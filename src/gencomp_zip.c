// ------------------------------------------------------------------
//   gencomp.c - "generated component"
//   Copyright (C) 2021-2025 Genozip Limited. Patent pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

#include <errno.h>
#include <pthread.h>
#include "libdeflate_1.19/libdeflate.h"
#include "gencomp.h"
#include "zip.h"
#include "codec.h"
#include "mgzip.h"
#include "biopsy.h"
#include "stream.h"
#include "dispatcher.h"
#include "sam_private.h"

//-----------------------
// Types & macros
//-----------------------

#define MAX_QUEUE_SIZE_BITS 16 // fields in queue and QBits are 16 bits
#define MAX_QUEUE_SIZE ((1 << MAX_QUEUE_SIZE_BITS)-2)
#define END_OF_LIST (MAX_QUEUE_SIZE+1)

typedef struct {
    BufferP gc_txts;         // Buffer array - each buffer is either on the "ready-to-dispatch queue" or on the "unused buffers' stack"
    uint16_t queue_size;     // number of buffers allocated on queue
    volatile uint16_t queue_len;  // number of buffers ready for dispatching (NOT including buffers offloaded to disk)
    uint16_t next_to_leave;  // Head of queue (FIFO): doubly-linked list of buffers 
    uint16_t last_added;     // Tail of queue 
    uint16_t next_unused;    // Top of stack: Unused gc_txts buffers stack (LIFO): linked-list of buffers
} QueueStruct;

// overlays Buffer.param of queue[gct].gc_txts[buf_i]
typedef struct {
    uint32_t num_lines : 27; // VB size is limited to 1GB and VCF spec mandates at least 16 characters per line (8 mandatory fields), SAM mandates 22 (11 fields)
    uint32_t comp_i    : 2;
    uint32_t unused    : 3;
    uint16_t next;           // queue: towards head ; stack: down stack
    uint16_t prev;           // queue: towards tail ; stack: always END_OF_LIST
} QBits;

typedef union {
    QBits bits;
    int64_t value;
} QBitsType;
#define GetQBit(buf_i,field) ((QBitsType){ .value = queueP[gct].gc_txts[buf_i].param }.bits.field)
#define SetQBit(buf_i,field) ((QBitsType*)&queueP[gct].gc_txts[buf_i].param)->bits.field
#define SetQBits(buf_i)      ((QBitsType*)&queueP[gct].gc_txts[buf_i].param)->bits

typedef struct {
    uint32_t comp_len;
    uint32_t num_lines;
    CompIType comp_i;
} TxtDataInfoType;

typedef struct {
    char *name;          
    FILE *fp;   
    Buffer offload_info; // array of TxtDataInfoType - one entry per txt_data buffer written to the file. 
    Buffer thread_data;
    Buffer thread_data_comp;
    bool has_thread;
    pthread_t thread;
} DepnStruct;

typedef struct  {
    GencompType type;
    Buffer txt_data;     // we absorb gencomp lines separately for each component, and then flush a single-component VB to the queue - the queue may have VBs from several components
} CompStruct;

// Thread safety model:
// At most 2 threads can access our data structures concurrently:
// "Absorb thread" - this is either one of the compute threads (selected using a serializing VB) or the main thread 
// for the final flush, after all compute threads are done absorbing. This thread accesses only the "Absorbing functions".
// "Dispatcher thread" - called by the main thread from zip_prepare_one_vb_for_dispatching, calling "Dispatcher functions".
// After all VBs are absorbed, a final call to flush occurs from the main thread, it is the only thread running and is considered
// to be both "Absorb thread" and "Dispatcher thread".
//
// gc_protected protects variables that may be accessed by both "Absorbing functions" and "Dispatcher functions" running concurrently.
static Mutex gc_protected = {};

// --------------------------------------------------------------------------------------
// unprotected data structures - accessed only by "Dispatcher functions" (main thread)
// --------------------------------------------------------------------------------------
static bool sam_finished_ingesting_prim = false;
static VBIType num_SAM_PRIM_vbs_ingested = 0;
static VBIType num_vbs_dispatched[NUM_GC_TYPES] = {}; // by gencomp type
static DepnStruct depn = {};              // doesn't need protection because accessed by Dispatcher functions only after Absorption is complete

// ZIP: two methods for handling DEPN lines that are collected when segging the MAIN component, and need
// to be segged later, after all MAIN and PRIM VBs are segged.
static enum { DEPN_NONE,            
              DEPN_OFFLOAD, // DEPN lines are offloaded to disk (compressed with RANS), to be later read back
              DEPN_REREAD   // DEPN lines are re-read from txt file
            } depn_method;  // immutable after initialization in gencomp_initialize

// --------------------------------------------------------------------------------------
// Mutex-protected data structures - accessed by both "Dispatcher functions" (main thread)
// and "Absorbing functions" (serialized compute thread) running concurrently (veriable names have a 'P' suffix)
// --------------------------------------------------------------------------------------
#define MAX_GEN_COMP 2
static bool finished_absorbingP = false;
static VBIType num_MAIN_vbs_absorbedP = 0; 
static uint64_t num_lines_absorbed[MAX_GEN_COMP+1]= {};
static QueueStruct queueP[NUM_GC_TYPES] = {}; // queue of txt_data's [1] out-of-band (used for SAM PRIM) [2] DEPN (used for SAM DEPN)
static CompStruct componentsP[MAX_GEN_COMP+1] = {};

#define GC_TXTS_BUF_NAME "queueP.gc_txts"

// --------------------------------------------------------------------------------------
// unprotected data is accessed by absorbing threads until absorbing is done, 
// and after by dispatcher functions
// --------------------------------------------------------------------------------------

typedef struct { 
    RereadLine *uncomp;  // uncompress array of ReReadline as extracted from reread_current_prescription
    char *uncomp_memory; // memory containing uncomp should be freed after uncomp is compressed 
    char *comp;          // a compressed array of RereadLine, representing lines that in total are <= segconf.vb_size
    uint32_t comp_len;   // length of comp (in bytes)
    uint32_t num_lines;  // length of uncomp array (in units of ReReadline)
    uint32_t txt_len;    // txt_data length of the lines listed 
} DepnVBPrescription, *DepnVBPrescriptionP;

// array of DepnVBPrescription - one for DEPN vb. The buffer is immutable after all MAIN VBs are absorbed.
static Buffer reread_depn_vb_prescriptions = {}; 
static Mutex prescriptions_mutex = {}; // protects access to reread_depn_vb_prescriptions

// used during absorbing MAIN VBs: an array of (uncompressed) RereadLine of the current prescription being created
static Buffer reread_current_prescription = {}; // access is protected by gc_protected.  .count is the txt_data length represented by the lines listed in reread_current_prescription

static VBlockP compress_depn_vb = NULL;

//--------------------------------------------------
// Seg: adding gencomp lines to vb->gencomp_lines
//--------------------------------------------------

// ZIP compute thread: store location where this gc line should be inserted    
void gencomp_seg_add_line (VBlockP vb, CompIType comp_i, STRp(line)/*pointer into txt_data*/)
{
    ASSERT (line_len <= GENCOMP_MAX_LINE_LEN, "line_len=%u is beyond maximum of %u", line_len, GENCOMP_MAX_LINE_LEN);
    
    buf_alloc (VB, &vb->gencomp_lines, 1, 5000, GencompLineIEntry, 2, "gencomp_lines");
 
    BNXT (GencompLineIEntry, vb->gencomp_lines) = 
        (GencompLineIEntry){ .line_i     = vb->line_i, 
                             .line_index = BNUMtxt (line),
                             .line_len   = line_len,
                             .comp_i     = comp_i };

    // If we're might re-read depn lines from the txt file, we store their coordinates in the txt file
    if (componentsP[comp_i].type == GCT_DEPN && depn_method == DEPN_REREAD) {
        if (TXT_IS_BGZF) {
            uint64_t bb_i = vb->vb_mgzip_i + vb->gz_blocks.current_bb_i;
            ASSERT (bb_i <= MAX_BB_I, "%s: BGZF bb_i=%"PRIu64" exceeds maximum of %"PRIu64, VB_NAME, bb_i, MAX_BB_I);

            BLST (GencompLineIEntry, vb->gencomp_lines)->offset = (LineOffset){ .bb_i = bb_i, .uoffset = vb->line_bgzf_uoffset };
        }
        else // CODEC_NONE
            BLST (GencompLineIEntry, vb->gencomp_lines)->offset = (LineOffset){ .offset = vb->vb_position_txt_file + BNUMtxt (line) };
    }
}

//--------------------------------------------------
// Misc functions
//--------------------------------------------------

// true if VB belongs to a generated componentsP
bool gencomp_comp_eligible_for_digest (VBlockP vb)
{
    CompIType comp_i = vb ? vb->comp_i : flag.zip_comp_i;
    DataType dt = vb ? vb->data_type : z_file->data_type;

    return (comp_i == COMP_MAIN) || // The MAIN component is always digestable 
           (dt == DT_FASTQ)      || // FASTQ components are alway digestable (including FQ_COMP_R2, SAM_COMP_FQXX) 
           ((dt == DT_SAM || dt == DT_BAM) && comp_i >= SAM_COMP_FQ00); // works even when vb=NULL
}

static void debug_gencomp (rom msg, bool needs_lock, VBlockP vb/*optional*/)
{
    if (needs_lock) mutex_lock (gc_protected); 

    QueueStruct *q1 = &queueP[GCT_OOB], *q2 = &queueP[GCT_DEPN];

    iprintf ("%-12.12s PRIM Queue: size=%u len=%u tail=%-2d head=%-2d unused=%-2d DEPN Queue: S=%u L=%u T=%-2d H=%-2d U=%-2d #on_disk=%u%s%s\n",
             msg, 
             q1->queue_size, q1->queue_len, (int16_t)q1->last_added, (int16_t)q1->next_to_leave, (int16_t)q1->next_unused,
             q2->queue_size, q2->queue_len, (int16_t)q2->last_added, (int16_t)q2->next_to_leave, (int16_t)q2->next_unused, (int)depn.offload_info.len,
             cond_int (vb, " vb_i=", vb->vblock_i),
             cond_int (vb, " lines=", vb->lines.len32));

    if (needs_lock) mutex_unlock (gc_protected);
}

//-----------------------------------------------------------------------------
// Initalization and finalization - called by main thread outside of dispatcher
// loop - no compute threads are running
//-----------------------------------------------------------------------------

// main thread
void gencomp_initialize (CompIType comp_i, GencompType gct) 
{
    // case: first call for this z_file
    componentsP[COMP_MAIN] = (CompStruct){ .type = GCT_NONE }; 
    componentsP[comp_i]    = (CompStruct){ .type = gct      };

    if (!gc_protected.initialized) 
        mutex_initialize (gc_protected);

    // add to buffer list. we can't allocate yet because segconf.vb_size is not known yet
    buf_set_promiscuous (&componentsP[comp_i].txt_data, "componentsP[]");

    // initialize queue. note: same-type components share a queue
    #if MAX_QUEUE_SIZE < MAX_GLOBAL_MAX_THREADS 
    #error MAX_GLOBAL_MAX_THREADS must be at most MAX_QUEUE_SIZE or else the queue_size might overflow
    #endif

    if (gct == GCT_DEPN) {

        buf_set_promiscuous (&depn.thread_data, "depn.thread_data");
        buf_set_promiscuous (&depn.thread_data_comp, "depn.thread_data_comp");

        // if we cannot re-read the depn lines from the file, we will offload them to disk
        if ((!TXT_IS_BGZF && !TXT_IS_PLAIN) || 
             txt_file->redirected || txt_file->is_remote ||
             segconf.zip_txt_modified) { 
            depn_method = DEPN_OFFLOAD;

            int depn_name_size = strlen (z_file->name) + 20;
            depn.name = MALLOC (depn_name_size);
            snprintf (depn.name, depn_name_size, "%s.DEPN", z_file->name);
            buf_set_promiscuous (&depn.offload_info, "depn.offload_info");

            ASSINP (!flag.force_reread, "--force-reread not supported: redirected=%s remote=%s modified=%s effective_codec=%s",
                    TF(txt_file->redirected), TF(txt_file->is_remote), TF(segconf.zip_txt_modified), codec_name (txt_file->effective_codec));
        }

        else {
            depn_method = DEPN_REREAD;
            buf_set_promiscuous (&reread_depn_vb_prescriptions, "reread_depn_vb_prescriptions");
            buf_set_promiscuous (&reread_current_prescription,  "reread_current_prescription");
        
            buf_alloc (evb, &reread_depn_vb_prescriptions, 0, 1000, DepnVBPrescription, 0, NULL);
            buf_alloc (evb, &reread_current_prescription, 0, 1.15 * (double)segconf.vb_size / segconf.line_len, RereadLine, 0, NULL);
        
            mutex_initialize (prescriptions_mutex);
        }
    }

    if (!queueP[gct].gc_txts) {
        queueP[gct] = (QueueStruct) {
            // the OOB queue might grow in case a VB is slow to seg and many VBs after it are pre-absorbed. then when
            // the slow VB is finally done, all the pre-absorbed VBs are added to the OOB queue at once, before the dispatcher
            // has a chance to consume any data. We observed cases of the queue growing 3X the number of threads, so 7X
            // should be sufficient. If it still overflows, flush will fail, resulting in an over-size OOB (=SAM PRIM) VB. no harm.
            .queue_size    = (gct == GCT_OOB)             ? global_max_threads * 7 // OOB 
                           : (depn_method == DEPN_REREAD) ? global_max_threads // DEPN: when re-reading from txt_file data that doesn't fit into the queue - a larger queue is beneficial
                           :                                1,                 // DEPN: when writing a file with the data that doesn't fit into the queue - No strong benefit of having more buffers in memory, see: private/internal-docs/gencomp-depn-memory-queue-vs-disk.txt
            .next_unused   = 0,           // 0 means gc_txts[0]
            .last_added    = END_OF_LIST, // ready-to-dispatch queue is initially empty
            .next_to_leave = END_OF_LIST
        };

        queueP[gct].gc_txts = CALLOC (sizeof (Buffer) * queueP[gct].queue_size);

        // add to evb buf_list, so we can buf_alloc in other threads (similar to INIT in file.c)
        for (int i=0; i < queueP[gct].queue_size; i++) 
            buf_set_promiscuous (&queueP[gct].gc_txts[i], GC_TXTS_BUF_NAME);

        // add all buffers to "unused stack"
        for (int i=0; i < queueP[gct].queue_size-1; i++) {
            SetQBit (i, next) = i+1;
            SetQBit (i, prev) = END_OF_LIST; // always EOL for stack
        }

        SetQBit (queueP[gct].queue_size-1, next) = END_OF_LIST;
    }
}

// main thread
void gencomp_destroy (void)
{
    buflist_destroy_bufs_by_name (GC_TXTS_BUF_NAME, false); 

    for (GencompType gct=1; gct < NUM_GC_TYPES; gct++)
        FREE (queueP[gct].gc_txts);

    for (CompIType comp_i=1; comp_i <= MAX_GEN_COMP; comp_i++)
        buf_destroy (componentsP[comp_i].txt_data);

    if (depn.fp) {
        fclose (depn.fp);
        remove (depn.name);
    }
    FREE (depn.name);
    buf_destroy (depn.offload_info);
    buf_destroy (depn.thread_data);
    buf_destroy (depn.thread_data_comp);
    buf_destroy (reread_depn_vb_prescriptions);
    buf_destroy (reread_current_prescription);

    vb_destroy_vb (&compress_depn_vb);

    mutex_destroy (gc_protected);
    mutex_destroy (prescriptions_mutex);
    memset (queueP,   0, sizeof (queueP));
    memset (componentsP, 0, sizeof (componentsP));
    memset (&depn, 0, sizeof (depn));
    memset ((void*)num_vbs_dispatched, 0, sizeof(num_vbs_dispatched));
    memset (&num_lines_absorbed, 0, sizeof (num_lines_absorbed));

    finished_absorbingP = sam_finished_ingesting_prim = false;
    num_SAM_PRIM_vbs_ingested = num_MAIN_vbs_absorbedP = 0;
    depn_method = DEPN_NONE;
}

//-----------------------------------------------------------------------------------------------
// "Absorbing functions" - Absorbing data from VBs into gencomp queues - these are called by
// the Absorb thread (for the final flush, the Absorbing and Dispatching is the same thread)
//-----------------------------------------------------------------------------------------------

static uint32_t compress_depn_buf (BufferP comp_buf)
{
    START_TIMER;

    compress_depn_vb = vb_initialize_nonpool_vb (VB_ID_COMPRESS_DEPN, DT_NONE, "compress_depn_buf");
    
    uint32_t uncomp_len = depn.thread_data.len32;
    uint32_t comp_len = codec_RANB_est_size (CODEC_RANB, uncomp_len);

    buf_alloc (evb, comp_buf, 0, comp_len + sizeof (uint32_t), char, 1, NULL); // added to evb's buf_list before
    *B1ST32 (*comp_buf) = uncomp_len;

    // about 3X compression (BAM) with no impact on time (compression+decompression time is a wash with the saving of I/O time)
    codec_RANB_compress (compress_depn_vb, NULL, NULL, depn.thread_data.data, &uncomp_len, NULL, comp_buf->data + sizeof(uint32_t), 
                         &comp_len, false, NULL);
    
    vb_release_vb (&compress_depn_vb, "compress_depn_buf");
    
    COPY_TIMER_EVB (compress_depn_buf);
    return (comp_buf->len = comp_len + sizeof (uint32_t));
}

static void *gencomp_do_offload (void *info_)
{
    TxtDataInfoType *info = (TxtDataInfoType *)info_;
    uint32_t uncomp_len = depn.thread_data.len32;
    info->comp_len = compress_depn_buf (&depn.thread_data_comp);

    START_TIMER;
    ASSERT (1 == fwrite (STRb(depn.thread_data_comp), 1, depn.fp), 
            "Failed to write %"PRIu64" bytes to temporary file %s: %s", depn.thread_data_comp.len, depn.name, strerror (errno));
    COPY_TIMER_EVB (gencomp_do_offload_write);

    if (flag_debug_gencomp) 
        iprintf ("Wrote to disk: buf=%u num_lines=%u uncomp_len=%u comp_len=%u uncomp_alder32=%u comp_adler32=%u\n",
                 BNUM (depn.offload_info, info), info->num_lines, uncomp_len, info->comp_len, 
                 adler32 (1, STRb(depn.thread_data)), adler32 (1, STRb(depn.thread_data_comp))); 

    depn.thread_data_comp.len = depn.thread_data.len = 0;

    return NULL;
}

// called by gencomp_flush with gc_protected locked. 
static void gencomp_offload_DEPN_to_disk (CompIType comp_i, bool is_final_flush)
{
    START_TIMER;

    // open file upon first write 
    if (!depn.fp) {
        depn.fp = fopen (depn.name, WRITEREAD); 
        ASSERT (depn.fp, "fopen() failed to open %s: %s", depn.name, strerror (errno));

        // unlink from directory so file gets deleted on close and hopefully also prevents unnecessary flushing
        unlink (depn.name); // ignore errors (this doesn't work on NTFS)
    }

    BufferP buf = &componentsP[comp_i].txt_data;

    buf_alloc (evb, &depn.offload_info, 1, 100, TxtDataInfoType, 2, "depn.offload_info");
    BNXT (TxtDataInfoType, depn.offload_info) = (TxtDataInfoType){
        .comp_i    = comp_i,
        .num_lines = buf->count
        // .comp_len is set by gencomp_do_offload
    };
    TxtDataInfoType *info = BLST (TxtDataInfoType, depn.offload_info);

    // copy data to another buffer, as this one will be freed and recycled
    buf_copy (evb, &depn.thread_data, buf, uint8_t, 0, 0, NULL);

    int err;
    if (!is_final_flush && global_max_threads > 1) {
        // compress and write to disk in separate thread, as to not delay releasing this VB. Only one thread is run in parallel as we join
        // the previous before creating a new one.
        ASSERT (!(err = pthread_create (&depn.thread, NULL, gencomp_do_offload, info)), 
                "failed to create gencomp_do_offload thread: %s", strerror(err));
        
        depn.has_thread = true;
    }
    else 
        gencomp_do_offload(info);

    if (flag_debug_gencomp) debug_gencomp ("offloaded DEPN", false, NULL);

    COPY_TIMER_EVB (gencomp_offload_DEPN_to_disk);
}

static void *gencomp_compress_depn (void *comp_buf)
{
    compress_depn_buf ((BufferP)comp_buf);
    depn.thread_data_comp.len = depn.thread_data.len = 0;

    return NULL;
}

// ZIP: called with gc_protected locked.
// moves data from componentsP.txt_data where it was absorbing lines from MAIN VBs to the GetQBit or DEPN queue
static bool gencomp_flush (CompIType comp_i, bool is_final_flush) // final flush of gct, not of comp_i!
{
    START_TIMER;

    if (!componentsP[comp_i].txt_data.len) return true; // nothing to flush

    GencompType gct = componentsP[comp_i].type;

    // pop a buffer from the unused stack
    uint16_t buf_i = queueP[gct].next_unused;
 
    // sanity
    ASSERT ((buf_i == END_OF_LIST) == (queueP[gct].queue_len == queueP[gct].queue_size), 
            "Queue is broken: buf_i=%d END_OF_LIST=%d queueP[%u].queue_len=%u queueP[%u].queue_size=%u",
            buf_i, END_OF_LIST, gct, queueP[gct].queue_len, gct, queueP[gct].queue_size);

    // wait for previous DEPN compression / offload to finish
    if (gct == GCT_DEPN && depn.has_thread) {
        int err;
        ASSERT (!(err = PTHREAD_JOIN (depn.thread, "gencomp_compress_depn")), "pthread_join failed: %s", strerror (err));
        depn.has_thread = false;
    }

    // if there are no more room on the queue - offload to disk (only if GCT_DEPN)
    if (buf_i == END_OF_LIST) {
        // case: no room for flushing. see comment in gencomp_initialize as to how this might happen. 
        // caller should just continue to grow componentsP[comp_i].txt_data resulting in an over-sized OOB (=SAM PRIM) VB
        if (gct == GCT_OOB) return false; 

        // case: DEPN - we offload one buffer from the queue to the file
        gencomp_offload_DEPN_to_disk (comp_i, is_final_flush);
    }

    else {
        queueP[gct].next_unused = is_final_flush ? END_OF_LIST : GetQBit(buf_i, next);

        // add the buffer at the tail of the queue
        if (queueP[gct].last_added != END_OF_LIST)
            SetQBit (queueP[gct].last_added, prev) = buf_i;

        SetQBits(buf_i) = (QBits){ .next      = queueP[gct].last_added,
                                   .prev      = END_OF_LIST,
                                   .num_lines = componentsP[comp_i].txt_data.count,
                                   .comp_i    = comp_i } ;
        
        queueP[gct].last_added = buf_i;
        queueP[gct].queue_len++;

        if (queueP[gct].next_to_leave == END_OF_LIST) {
            ASSERT0 (queueP[gct].queue_len==1, "Expecting that if queue's head==tail, then queue_len=1"); // sanity
            queueP[gct].next_to_leave = buf_i;
        }

        // OOB: copy data to the buffer we added to the GetQBit queueP - dispatcher will consume it next, so queue
        // normally doesn't grow more than a handful of buffers
        if (gct == GCT_OOB) {
            buf_alloc (evb, &queueP[gct].gc_txts[buf_i], 0, segconf.vb_size, char, 0, NULL); // allocate to full size of VB so it doesn't need to be realloced
            buf_copy (evb, &queueP[gct].gc_txts[buf_i], &componentsP[comp_i].txt_data, char, 0, 0, 0);
        }

        // DEPN: data is only consumed after all MAIN and OOB data is consumed, so queue might grow very large.
        // We compress it so we can have more in memory and less offloaded to disk
        else {
            // copy data to another buffer, as this one will be freed and recycled
            buf_copy (evb, &depn.thread_data, &componentsP[comp_i].txt_data, uint8_t, 0, 0, NULL);

            int err;
            if (!is_final_flush && global_max_threads > 1) {
                // compress in separate thread, as to not delay releasing this VB. Only one thread is run in parallel as we join
                // the previous before creating a new one.
                ASSERT (!(err = pthread_create (&depn.thread, NULL, gencomp_compress_depn, &queueP[gct].gc_txts[buf_i])), 
                        "failed to create gencomp_compress_depn thread: %s", strerror(err));
                
                depn.has_thread = true;
            }
            else 
                gencomp_compress_depn (&queueP[gct].gc_txts[buf_i]);
        }
    }

    componentsP[comp_i].txt_data.len = componentsP[comp_i].txt_data.param = 0; // reset

    if (flag_debug_gencomp) debug_gencomp(comp_i==1 ? "Flush PRIM" : "Flush DEPN", false, NULL);

    COPY_TIMER_EVB (gencomp_flush);
    return true;
}

// called from gencomp_depn_vb_prescriptions_memory - on memory exception or SIGUSR1
uint64_t gencomp_depn_vb_prescriptions_memory (void)
{
    mutex_lock (prescriptions_mutex); // mostly to avoid a realloc under our feet

    uint64_t memory=0;

    for_buf (DepnVBPrescription, pres, reread_depn_vb_prescriptions)
        memory += pres->comp_len ? pres->comp_len : (pres->num_lines * sizeof (RereadLine));

    mutex_unlock (prescriptions_mutex);

    return memory;
}

// called by compute thread, while gc_protected is locked
static int32_t gencomp_rotate_prescription (void)
{
    mutex_lock (prescriptions_mutex); 

    if (flag_debug_gencomp) 
        iprintf ("Rotating DEPN reread_current_prescription: txt_size=%u, num_lines=%u\n",
                 (uint32_t)reread_current_prescription.count, reread_current_prescription.len32);

    buf_alloc (NULL, &reread_depn_vb_prescriptions, 1, 0, DepnVBPrescription, 2, NULL);

    int32_t pres_i = reread_depn_vb_prescriptions.len32;
    DepnVBPrescriptionP pres = &BNXT (DepnVBPrescription, reread_depn_vb_prescriptions);

    // removes the memory from the buffer and replenishes it with new memory
    buf_extract_data (reread_current_prescription, (char **)&pres->uncomp, 0, &pres->num_lines, &pres->uncomp_memory);

    pres->txt_len = reread_current_prescription.count; // counts txt_data length represented by the lines in reread_current_prescription
    reread_current_prescription.count = 0; 

    mutex_unlock (prescriptions_mutex);

    return pres_i;
}

static void gencomp_compress_prescription (VBlockP vb, int32_t pres_i)
{
    // copy prescription
    mutex_lock (prescriptions_mutex); 
    DepnVBPrescription pres = *B(DepnVBPrescription, reread_depn_vb_prescriptions, pres_i);
    mutex_unlock (prescriptions_mutex);

    // deltify
    if (TXT_IS_BGZF)
        for (int32_t i=pres.num_lines - 1; i >= 1; i--) {
            pres.uncomp[i].offset.bb_i -= pres.uncomp[i-1].offset.bb_i;
            if (!pres.uncomp[i].offset.bb_i) // same bb_i
                pres.uncomp[i].offset.uoffset -= pres.uncomp[i-1].offset.uoffset;
        }
    
    else
        for (int32_t i=pres.num_lines - 1; i >= 1; i--)
            pres.uncomp[i].offset.offset -= pres.uncomp[i-1].offset.offset;

    // compress outside of mutex
    ASSERTNOTINUSE (vb->scratch);
    uint32_t uncomp_len = pres.num_lines * sizeof(RereadLine);

    pres.comp_len = codec_RANW_est_size (CODEC_RANW, uncomp_len);
    buf_alloc_exact (vb, vb->scratch, pres.comp_len, char, "scratch");

    // compress into vb->scratch: note: RANW is about 4-5x. LZMA is usually better, but takes 7x time.
    codec_RANW_compress (vb, NULL, NULL, (rom)pres.uncomp, &uncomp_len, NULL, B1STc(vb->scratch), &pres.comp_len, HARD_FAIL, "scratch");

    buf_free (vb->codec_bufs[0]);

    pres.comp = MALLOC (pres.comp_len); // usually a lot less than alloced to scratch
    memcpy (pres.comp, B1STc(vb->scratch), pres.comp_len);

    FREE (pres.uncomp_memory);
    pres.uncomp = NULL;
    buf_free (vb->scratch);

    // update consumed prescription
    mutex_lock (prescriptions_mutex); 
    *B(DepnVBPrescription, reread_depn_vb_prescriptions, pres_i) = pres;
    mutex_unlock (prescriptions_mutex);
}

// Called from compute_vb, for MAIN VBs, in arbitrary order of VBs. 
// Note: We need to do it in the compute thread (rather than the main thread) so that zip_prepare_one_vb_for_dispatching, 
// running in the main thread, can busy-wait for all MAIN compute threads to complete before flushing the final txt_data.
void gencomp_absorb_vb_gencomp_lines (VBlockP vb) 
{
    START_TIMER; // not including mutex wait times

    mutex_lock (gc_protected);

    uint64_t first_prim = num_lines_absorbed[SAM_COMP_PRIM];
    uint64_t first_depn = num_lines_absorbed[SAM_COMP_DEPN];
    int32_t prescription_rotated = -1;

    if (vb->gencomp_lines.len) {
        ASSERT0 (!finished_absorbingP, "Absorbing is done - not expecting to be called");
        
        // limit vb_size of depn to 64MB, to reduce the gencomp write bottleneck, esp in --best in files with lots of depn
        // also: a bug in rans_compress_to_4x16 (called from compress_depn_buf) erroring in buffers near 2GB.
        uint32_t comp_size[3] = { [1] = segconf.vb_size,
                                  [2] = componentsP[2].type == GCT_DEPN ? MIN_(segconf.vb_size, 64 MB) : segconf.vb_size };

        for (int i=1; i<=2; i++)
            buf_alloc (evb, &componentsP[i].txt_data, 0, comp_size[i], char, 1, "componentsP.txt_data");    

        // iterate on all lines the segmenter decided to send to gencomp and place each in the correct queue
        for_buf (GencompLineIEntry, gcl, vb->gencomp_lines) {

            // flush previous lines if there is no room for new line
            if (componentsP[gcl->comp_i].txt_data.len + gcl->line_len > comp_size[gcl->comp_i] && !flag.force_reread) {
                bool flushed = gencomp_flush (gcl->comp_i, false);

                // case: not flushed bc too much data is already waiting for the dispatcher - just continue to grow this txt_data - 
                // we will have an over-sized VB. See details of how this might happen in comment in gencomp_initialize
                if (!flushed) 
                    buf_alloc (NULL, &componentsP[gcl->comp_i].txt_data, gcl->line_len, 0, char, 1.5, NULL);
            }

            // note: it is possible that the some lines will fit into the queue, while subsequent
            // lines will be slated for re-reading 
            if (depn_method == DEPN_REREAD && 
                componentsP[gcl->comp_i].type == GCT_DEPN && 
                (queueP[GCT_DEPN].next_unused == END_OF_LIST || flag.force_reread)) {

                if (reread_current_prescription.count + gcl->line_len > segconf.vb_size) {
                    ASSERT (prescription_rotated == -1, "%s: only one DEPN prescription can be rotated per MAIN VB. reread_current_prescription.count=%"PRIu64", segconf.vb_size=%u gcl->line_len=%u", 
                            VB_NAME, reread_current_prescription.count, (int)segconf.vb_size, gcl->line_len); // since MAIN VB is also at most segconf.vb_size, even if ALL lines in MAIN_VB are DEPNs, they are still under vb_size, so at most one rotation can occur
                    prescription_rotated = gencomp_rotate_prescription ();
                }

                buf_append_one (reread_current_prescription, ((RereadLine){ .offset = gcl->offset, .line_len = gcl->line_len }));
                reread_current_prescription.count += gcl->line_len;
            }

            // to componentsP.txt_data to be flushed to the queue later
            else {
                buf_add_do (&componentsP[gcl->comp_i].txt_data, Btxt(gcl->line_index), gcl->line_len);
                componentsP[gcl->comp_i].txt_data.count++; 
            }

            num_lines_absorbed[gcl->comp_i]++;
        }
    }

    sam_add_main_vb_info (vb, first_prim, num_lines_absorbed[SAM_COMP_PRIM] - first_prim, 
                              first_depn, num_lines_absorbed[SAM_COMP_DEPN] - first_depn);

    if (flag_debug_gencomp) 
        iprintf ("%s absorbed %u gencomp lines: prim={start=%"PRIu64" len=%"PRIu64"} depn={start=%"PRIu64" len=%"PRIu64"}\n", 
                 VB_NAME, vb->gencomp_lines.len32, first_prim, num_lines_absorbed[SAM_COMP_PRIM]-first_prim, first_depn, num_lines_absorbed[SAM_COMP_DEPN]-first_depn);

    // we declare the file has having gencomp only if we actually have gencomp data 
    if (vb->gencomp_lines.len)
        z_file->z_flags.has_gencomp = true; 

    num_MAIN_vbs_absorbedP++;

    mutex_unlock (gc_protected);

    if (prescription_rotated != -1)
        gencomp_compress_prescription (vb, prescription_rotated); // outside of mutex

    COPY_TIMER (gencomp_absorb_vb_gencomp_lines);
}

// main thread: called after all VBs (i.e. num_lines_absorbed values are final and stable)
bool gencomp_have_any_lines_absorbed (void)
{
    return num_lines_absorbed[SAM_COMP_PRIM] + num_lines_absorbed[SAM_COMP_DEPN] > 0;
}
//------------------------------------------------------------------------------------
// "Dispatcher functions" - Main thread functions called from within the dispatcher 
// loop - running in parallel with "Absorbing functions"
//------------------------------------------------------------------------------------

// main thread - called with gc_protected locked. remove the "next to leave" from the queue and place it
// on the unused stack. Caller should reset the buffer's len after copying it elsewhere. called with gc_protect locked.
static int gencomp_buf_leaves_queue (GencompType gct)
{
    int buf_i = queueP[gct].next_to_leave;

    // remove from buffer from queue
    queueP[gct].next_to_leave = GetQBit(buf_i, prev);
    if (queueP[gct].next_to_leave != END_OF_LIST) 
        SetQBit(queueP[gct].next_to_leave, next) = END_OF_LIST;
    else
        queueP[gct].last_added = END_OF_LIST; // buf_i was the head AND the tail of the list

    // add buffer to unused stack
    SetQBit(buf_i, next) = queueP[gct].next_unused;
    SetQBit(buf_i, prev) = END_OF_LIST; // always EOL for stack

    queueP[gct].next_unused = buf_i;

    return buf_i;
}

// main thread - called with gc_protected locked
static void gencomp_get_txt_data_from_queue (VBlockP vb, GencompType gct)
{
    int buf_i = gencomp_buf_leaves_queue (gct);
    BufferP buf = &queueP[gct].gc_txts[buf_i];

    if (gct == GCT_OOB)
        buf_copy (vb, &vb->txt_data, buf, char, 0, 0, "txt_data");
    else {
        // compressed buffer: first 4 bytes are uncomp_len, then the compressed data
        Ltxt = *B1ST32 (*buf);
        buf_alloc (vb, &vb->txt_data, 0, segconf.vb_size, char, 0, "txt_data"); 
    
        codec_rans_uncompress (evb, NULL, CODEC_RANB, 0, buf->data + 4, buf->len - 4, 
                               &vb->txt_data, Ltxt, 0, "txt_data");
    }

    // initialize VB
    vb->comp_i    = GetQBit(buf_i, comp_i);
    vb->lines.len = GetQBit(buf_i, num_lines);    
    buf->len = 0;  // reset

    num_vbs_dispatched[gct]++;
    queueP[gct].queue_len--;

    if (flag_debug_gencomp) 
        debug_gencomp (vb->comp_i==1 ? "GetTxt PRIM" : "GetTxt DEPN", false, vb);

    if (flag_is_show_vblocks (ZIP_TASK_NAME)) 
        iprintf ("TXT_DATA_FROM_GENCOMP_QUEUE(id=%d) vb=%s buf_i=%u Ltxt=%u n_lines=%u\n", 
                 vb->id, VB_NAME, buf_i, Ltxt, vb->lines.len32);

    mutex_unlock (gc_protected);
} 

// main thread
static void gencomp_get_txt_data_from_disk (VBlockP vb)
{
    // case: we have data on disk - use it first
    if (!depn.offload_info.next)  // first read
        ASSERT (!fseek (depn.fp, 0, SEEK_SET), "fseek(%s,0) failed: %s", depn.name, strerror(errno));

    TxtDataInfoType *info = B(TxtDataInfoType, depn.offload_info, depn.offload_info.next++); 
    vb->comp_i         = info->comp_i;
    vb->lines.len      = info->num_lines;

    // copy data to VB
    buf_alloc (vb, &vb->txt_data, 0, segconf.vb_size, char, 0, "txt_data"); // allocate to full size of VB so it doesn't need to be realloced

    // note: thread_data_comp is already allocated, from when used for compression
    ASSERT (fread (depn.thread_data_comp.data, info->comp_len, 1, depn.fp) == 1, 
            "Failed to read buffer #%u length %u from %s", (int)depn.offload_info.next-1, (int)Ltxt, depn.name);
    depn.thread_data_comp.len = info->comp_len;

    Ltxt = *B1ST32 (depn.thread_data_comp); // first 4 bytes = uncomp_len
    ASSERT (Ltxt <= segconf.vb_size, "Invalid Ltxt=%u", Ltxt); // sanity

    codec_rans_uncompress (evb, NULL, CODEC_RANB, 0, depn.thread_data_comp.data+4, depn.thread_data_comp.len-4, 
                           &vb->txt_data, Ltxt, 0, "txt_data");

    if (flag_debug_gencomp) 
        iprintf ("Read from disk: buf=%"PRIu64" vb=%s num_lines=%u uncomp_len=%u comp_len=%u uncomp_alder32=%u comp_adler32=%u\n",
                  depn.offload_info.next-1, VB_NAME, info->num_lines, Ltxt, info->comp_len, 
                  adler32 (1, STRb(vb->txt_data)), adler32 (1, STRb(depn.thread_data_comp))); 

    depn.thread_data_comp.len = depn.thread_data.len = 0;

    if (flag_debug_gencomp) debug_gencomp ("ReadDisk DEPN", true, NULL);
}

// main thread - creates vb->reread_prescription with offsets of lines to be reread. the actual
// re-reading is done by the Seg compute thread for this vb
static void gencomp_prescribe_reread (VBlockP vb)
{
    DepnVBPrescriptionP pres = B(DepnVBPrescription, reread_depn_vb_prescriptions, reread_depn_vb_prescriptions.next++);
    
    buf_alloc_exact (vb, vb->reread_prescription, pres->num_lines, RereadLine, "reread_prescription"); 

    codec_rans_uncompress (vb, NULL, CODEC_RANW, 0, pres->comp, pres->comp_len, 
                           &vb->reread_prescription, pres->num_lines * sizeof (RereadLine), 0, "reread_prescription");

    vb->comp_i = SAM_COMP_DEPN;
    vb->lines.len32 = pres->num_lines;
    Ltxt = pres->txt_len;

    FREE (pres->comp);
    pres->comp_len = pres->num_lines = 0; // so gencomp_depn_vb_prescriptions_memory shows no memory consumed for this prescription

    // undeltify
    ARRAY (RereadLine, rr, vb->reread_prescription);
    if (TXT_IS_BGZF)
        for (uint32_t i=1; i < rr_len; i++) {
            if (!rr[i].offset.bb_i) // same bb_i
                rr[i].offset.uoffset += rr[i-1].offset.uoffset;

            rr[i].offset.bb_i += rr[i-1].offset.bb_i;
        }
    
    else
        for (uint32_t i=1; i < rr_len; i++)
            rr[i].offset.offset += rr[i-1].offset.offset;

    if (flag_debug_gencomp) debug_gencomp ("Reread DEPN", true, NULL);
}

// main thread: populate vb->txt_data with the next buffer on the out-of-band or DEPN queue
bool gencomp_get_txt_data (VBlockP vb)
{
    #define DEBUG_GENCOMP(msg) \
        if (flag_debug_gencomp) \
            iprintf ("Returning %s vb=%s/%u with txt_data: len=%u adler32=%u\n", \
                    (msg), comp_name (vb->comp_i), vb->vblock_i, Ltxt, adler32 (1, STRb(vb->txt_data))); 

    #define RETURN(msg, call)       \
        ( { call;                   \
            DEBUG_GENCOMP (msg);    \
            zip_init_vb (vb);       \
            biopsy_take (vb);       \
            return true; })

    if (!gc_protected.initialized) return false; // definitely no gencomp at all in this file (but if initialized, it doesn't always mean there is gencomp)

    mutex_lock (gc_protected);

    // case: we have no GetQBit data available, but MAIN data has all been dispatched (so no point returning
    // because there is no more file data to read), and some MAIN VBs have not sent data to absorb yet - 
    // 
    // case: we have out-of-band data: send it first
    if (queueP[GCT_OOB].queue_len) {
        if (!z_file->SA_CIGAR_chewing_vb_i && IS_SAG_SA) 
            z_file->SA_CIGAR_chewing_vb_i = vb->vblock_i; // this VB will chew cigars if needed

        RETURN ("OOB_Q", gencomp_get_txt_data_from_queue (vb, GCT_OOB)); // also unlocks mutex
    }

    // case: finished ingesting PRIM, and finish all (if any) disk-offloaded data. Now we can do the in-memory GCT_DEPN queue   
    if (sam_finished_ingesting_prim && queueP[GCT_DEPN].queue_len)
        RETURN ("DEPN_Q", gencomp_get_txt_data_from_queue (vb, GCT_DEPN));

    // we might have data on disk - but we will not be accessing the queue or protected data anymore
    // and we don't want to hold the mutex while reading from disk
    mutex_unlock (gc_protected);

    // case: finished ingesting PRIM and no more out-of-band data, and all MAIN data has been flushed (which also means txt_file reached EOF,
    // see zip_prepare_one_vb_for_dispatching) - so no more MAIN or GetQBit data will be available. time for DEPN data.
    if (sam_finished_ingesting_prim && depn.offload_info.next < depn.offload_info.len) 
        RETURN ("DISK", gencomp_get_txt_data_from_disk (vb));

    // case: finished ingesting PRIM and no more out-of-band data, and all MAIN data has been flushed (which also means txt_file reached EOF,
    // see zip_prepare_one_vb_for_dispatching) - so no more MAIN or GetQBit data will be available. time for DEPN data.
    if (sam_finished_ingesting_prim && reread_depn_vb_prescriptions.next < reread_depn_vb_prescriptions.len) 
        RETURN (TXT_IS_BGZF ? "REREAD_BGZF" : "REREAD_PLAIN", gencomp_prescribe_reread (vb));

    // no more data exists at this point OR we have GCT_DEPN, but not finished ingesting PRIM yet
    // DEBUG_GENCOMP ("NO_DATA_AVAILABLE"); // commenting out because there are too many of
    return false; // no data available now

    #undef RETURN
}

// main thread: true if there is data on gencomp queues, or there might be in the future. called by
// zip_prepare_one_vb_for_dispatching after txt_file is exhausted.
bool gencomp_am_i_expecting_more_txt_data (void)
{
    if (!gc_protected.initialized) return false; // no gencomp at all in this file

    mutex_lock (gc_protected);

    if (!finished_absorbingP && num_vbs_dispatched[GCT_NONE] == num_MAIN_vbs_absorbedP) {
        // final flush. at this point, our thread is considered to be both "Dispatching" and "Absorbing"
        for (CompIType comp_i=1; comp_i <= MAX_GEN_COMP; comp_i++) 
            gencomp_flush (comp_i, z_sam_gencomp || comp_i==MAX_GEN_COMP); // final flush of gct, not of comp_i
        
        if (flag_debug_gencomp) iprintf ("Finished absorbing: num_MAIN_vbs_absorbedP=%u\n", num_MAIN_vbs_absorbedP);
        finished_absorbingP = true;

        // rotate and compress final prescription - after this, prescriptions Buffer is immutable
        if (reread_current_prescription.len) {
            gencomp_rotate_prescription();
            gencomp_compress_prescription (evb, reread_depn_vb_prescriptions.len32-1);
        }
    }

    bool expecting = !finished_absorbingP || queueP[GCT_OOB].queue_len || queueP[GCT_DEPN].queue_len ||
                     reread_depn_vb_prescriptions.next < reread_depn_vb_prescriptions.len;

    if ((TXT_DT(SAM) || TXT_DT(BAM)) && finished_absorbingP && !queueP[GCT_OOB].queue_len && !num_vbs_dispatched[GCT_OOB]) {
        sam_finished_ingesting_prim = true;
        if (flag_debug_gencomp) iprint0 ("No PRIM VBs in this file\n");
    }

    mutex_unlock (gc_protected);
    return expecting;
} 

// main thread
void gencomp_a_main_vb_has_been_dispatched (void)
{
    if (!gc_protected.initialized) return; // no gencomp at all in this file

    num_vbs_dispatched[GCT_NONE]++;
}

// main thread - called be sam_zip_after_compute for PRIM VBs, in order of VBs. 
void gencomp_sam_prim_vb_has_been_ingested (VBlockP vb)
{
    num_SAM_PRIM_vbs_ingested++; // only accessed by this function    

    // note: we enforce the order between finished_absorbingP and queue_len in store, so we can be relaxed in load
    mutex_lock (gc_protected);
    bool my_finished_absorbing = finished_absorbingP; 
    uint16_t prim_queue_len = queueP[GCT_OOB].queue_len; // in SAM, GetQBit is PRIM
    mutex_unlock (gc_protected);

    if (flag_debug_gencomp) iprintf ("Ingested SA Groups of vb=%s\n", VB_NAME);

    // thread safety: 1. finished_absorbingP and  prim_queue_len these two are updated by the gencomp_absorb_vb_gencomp_lines 
    // which guarantees that if we have data, at least one of these two conditions will be true. 
    // 2. gencomp_get_txt_data ensures that if there is a VB being dispatched, it is accounted for in
    // at least one of: num_vbs_dispatched[GCT_OOB], queueP[GCT_OOB].queue_len
    if ((VB_DT(SAM) || VB_DT(BAM)) && my_finished_absorbing && !prim_queue_len && num_vbs_dispatched[GCT_OOB] == num_SAM_PRIM_vbs_ingested) {
        sam_sa_prim_finalize_ingest ();
        sam_finished_ingesting_prim = true;
        if (flag_debug_gencomp) iprintf ("Finished ingesting SAGs: num_SAM_PRIM_vbs_ingested=%u\n", num_SAM_PRIM_vbs_ingested);
    }
}

// ZIP: compute thread of a DEPN VB: actually re-reading data into txt_data according to vb->reread_prescription
void gencomp_reread_lines_as_prescribed (VBlockP vb)
{
    START_TIMER;

    buf_alloc_exact (vb, vb->txt_data, Ltxt, char, "txt_data");
    Ltxt = 0;

    // open a file handle private to this VB
    FILE *fp = fopen (txt_file->name, "rb");
    ASSERT (fp, "%s: Failed to open %s for rereading depn lines: %s", VB_NAME, txt_file->name, strerror(errno));

    stream_set_inheritability (fileno (fp), false); // Windows: allow file_remove in case of --replace

    if (flag_is_show_vblocks (ZIP_TASK_NAME)) 
        iprintf ("REREAD_DEPN(id=%d) vb=%s n_lines=%u effective_codec=%s\n", 
                 vb->id, VB_NAME, vb->reread_prescription.len32, codec_name (txt_file->effective_codec));

    if (TXT_IS_BGZF) 
        bgzf_reread_uncompress_vb_as_prescribed (vb, fp);

    else { // CODEC_NONE
        for_buf (RereadLine, line, vb->reread_prescription) {
            ASSERT (!fseeko64 (fp, line->offset.offset, SEEK_SET),
                    "%s: fseeko64 on %s failed while rereading depn lines at offset=%"PRIu64": %s", 
                    VB_NAME, txt_file->name, line->offset.offset, strerror(errno));
            
            ASSERT (fread (BAFTtxt, line->line_len, 1, fp) == 1,
                    "%s: fread of %u bytes at offset=%"PRIu64" from %s file_size=%"PRIu64" failed while rereading depn lines: %s", 
                    VB_NAME, line->line_len, line->offset.offset, txt_file->name, ({ fseek (fp, 0, SEEK_END); ftello64 (fp); }), 
                    strerror(ferror(fp)));

            Ltxt += line->line_len;
        }
    }

    fclose (fp);

    if (flag_debug_gencomp)
        iprintf ("%s: Reread %u gencomp lines from txt_file adler32=%u\n", 
                 VB_NAME, vb->reread_prescription.len32, adler32 (1, STRb(vb->txt_data)));

    COPY_TIMER (gencomp_reread_lines_as_prescribed);
}

bool gencomp_buf_locate_depn (void *unused, ConstBufferP buf)    
{                                                               
    return is_p_in_range (buf, &depn, sizeof (depn));                
}                                                       

bool gencomp_buf_locate_componentsP (void *unused, ConstBufferP buf)    
{                                                               
    return is_p_in_range (buf, componentsP, sizeof (componentsP));                
}                                                       

bool gencomp_buf_locate_queueP (void *unused, ConstBufferP buf)    
{            
    for (GencompType gct=1; gct < NUM_GC_TYPES; gct++)
        if (is_p_in_range (buf, queueP[gct].gc_txts, sizeof (Buffer) * queueP[gct].queue_size)) 
            return true;

    return false;
}

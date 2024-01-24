// ------------------------------------------------------------------
//   qname_filter.c
//   Copyright (C) 2023-2024 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "qname_filter.h"
#include "buffer.h"
#include "segconf.h"
#include "qname.h"
#include "file.h"

typedef struct {
    uint32_t hash; // hash of qname
    STRl(qname, SAM_MAX_QNAME_LEN+1);
} QnameFilterItem;
static Buffer qnames_filter = {};

static ASCENDING_SORTER (qname_filter_sort_by_hash, QnameFilterItem, hash)
static BINARY_SEARCHER (find_qname_in_filter, QnameFilterItem, uint32_t, hash, false)

// initialize qnames_filter and flag.qname_filter from file
void qname_filter_initialize_from_file (rom filename)
{
    flag.qname_filter = (filename[0] == '^') ? -1 : 1;

    if (flag.qname_filter == -1) filename++; // negative filter

    file_split_lines (filename, "qnames_file", VERIFY_ASCII);

    ARRAY_alloc (QnameFilterItem, qname, n_lines, true, qnames_filter, evb, "qnames_filter");
    for (int i=0; i < n_lines; i++) {
        if (!line_lens[i]) continue;

        // qname includes FASTQ "@" prefix - remove it
        bool has_prefix =(line_lens[i] && *lines[i] == '@');

        // ignore everything after the first space or tab        
        rom sep;
        if ((sep = memchr (lines[i], ' ',  line_lens[i]))) line_lens[i] = sep - lines[i]; // truncate at first space
        if ((sep = memchr (lines[i], '\t', line_lens[i]))) line_lens[i] = sep - lines[i]; // truncate at first tab

        qname[i].qname_len = MIN_(line_lens[i] - has_prefix, SAM_MAX_QNAME_LEN);

        memcpy (qname[i].qname, lines[i] + has_prefix, qname[i].qname_len);

        if (VER(15))
            qname_canonize (QNAME1, qSTRa(qname[i].qname)); // possibly reduces qname_len

        qname[i].hash = qname_calc_hash (QNAME1, STRa(qname[i].qname), unknown, false, NULL);
    }

    buf_destroy (data); // defined in file_split_lines

    qsort (STRb(qnames_filter), sizeof(QnameFilterItem), qname_filter_sort_by_hash);

    // remove dups (note: only consecutive dups are removed - if 2+ qnames have the same hash, dups might be interleaved and not removed)
    int next = 0;
    for (int i=0; i < n_lines; i++)
        if (qname[i].qname_len && qname[i].qname[0] && // not empty
            (!i || qname[i].hash != qname[i-1].hash || !str_issame (qname[i].qname, qname[i-1].qname))) { // not dup
            
            if (i != next) qname[next] = qname[i];
            next++;
        }
    qnames_filter.len32 = next;

    // display sorted list of (hash,qname) pairs (uncomment for debugging)
    // for_buf (QnameFilterItem, e, qnames_filter) iprintf ("hash=%u %s\n", e->hash, e->qname);
}

// initialize qnames_filter and flag.qname_filter from command line
void qname_filter_initialize_from_opt (rom opt)
{
    flag.qname_filter = (opt[0] == '^') ? -1 : 1;

    if (flag.qname_filter == -1) opt++; // negative filter

    str_split (opt, strlen(opt), 0, ',', str, false);

    ARRAY_alloc (QnameFilterItem, qname, n_strs, true, qnames_filter, evb, "qnames_filter");
    for (int i=0; i < n_strs; i++) {
        if (!str_lens[i] || str_lens[i] > SAM_MAX_QNAME_LEN) continue;

        qname[i].qname_len = str_lens[i];
        memcpy (qname[i].qname, strs[i], qname[i].qname_len);

        qname[i].hash = qname_calc_hash (QNAME1, STRa(qname[i].qname), unknown, false, NULL);
    }

    qsort (STRb(qnames_filter), sizeof(QnameFilterItem), qname_filter_sort_by_hash);

    // display sorted list of (hash,qname) pairs (uncomment for debugging)
    // for_buf (QnameFilterItem, e, qnames_filter) iprintf ("hash=%u %s\n", e->hash, e->qname);
}

bool qname_filter_does_line_survive (STRp(qname))
{
    ASSERT (qname_len <= SAM_MAX_QNAME_LEN, "qname=\"%.*s\" has length=%u longer than allowed by SAM spec=%u", STRf(qname), qname_len, SAM_MAX_QNAME_LEN);

    if (VER(15))
        qname_canonize (QNAME1, qSTRa(qname)); // possibly reduce qname_len

    uint32_t hash = qname_calc_hash (QNAME1, STRa(qname), unknown, false, NULL);
    QnameFilterItem *ent = binary_search (find_qname_in_filter, QnameFilterItem, qnames_filter, hash);

    bool found = false;

    for (; ent && ent < BAFT (QnameFilterItem, qnames_filter) && ent->hash == hash; ent++)
        if (str_issame (qname, ent->qname)) {
            found = true;
            break;
        }

    return (flag.qname_filter == 1) ? found : !found; // positive or negative filter
}

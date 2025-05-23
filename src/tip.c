// ------------------------------------------------------------------
//   tip.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "flags.h"
#include "arch.h"
#include "license.h"
#include "sam.h"
#include "file.h"

static bool dt_encountered[NUM_DATATYPES] = {};

void tip_dt_encountered (DataType dt)
{
    dt_encountered[dt] = true;
}

// called in ZIP, or sometimes in PIZ if ZIP ends to non-returnable --test. 
void tip_print (void)
{
    #define E(dt) dt_encountered[DT_##dt]

    if (flag.no_tip || // protect from spamming the user with more than one tip
        !license_allow_tip()) return; 

    StrTextLong acadmic_notice = license_academic_tip(); // empty unless acadmic license

    if (!is_info_stream_terminal) {
        if (acadmic_notice.s[0]) iprintf ("\n%s\n", acadmic_notice.s);
        return;
    }

    rom valid_tips[256];
    int n=0;

    if (acadmic_notice.s[0])
        for (int i=0; i < 5; i++)
            valid_tips[n++] = acadmic_notice.s; // 5X more likely than other tips

    valid_tips[n++] = "Interested in how Genozip works? See the paper: " PAPER2;
    valid_tips[n++] = "Tip: you can use Genozip to downsample your data, see: " WEBSITE_DOWNSAMPLING;
    valid_tips[n++] = "Tip: increase the security of your data by using Genozip's built-in encryption, see: " WEBSITE_ENCRYPTION;
    valid_tips[n++] = "Tip: with Genozip, you can archive entire directories, see: " WEBSITE_ARCHIVING;
    valid_tips[n++] = "Interested in seeing who else is using Genozip? Here: " WEBSITE_INSTITUTIONS;
    valid_tips[n++] = "Tip: genozip files are an excellent way to share and publish data - uncompressing genozip files is always free\n";
    valid_tips[n++] = "Tip: you can use Genozip to compress a file directly from a URL, see: " WEBSITE_GENOZIP;
    valid_tips[n++] = "Is Genozip useful? Help your colleagues by posting on your institution's bioinformatics forum\n";

    if (!(E(SAM) || E(BAM) || E(VCF) || E(BCF)))
        valid_tips[n++] = "Please take a moment now to make a note to not forget to cite Genozip:\n" PAPER2_CITATION "\n";

    if (dist_is_github())  
        valid_tips[n++] = "Do you like Genozip? Please support it by starring it on github: " GITHUB_REPO;

    if (E(SAM) || E(BAM) || E(FASTQ)) 
        valid_tips[n++] = "Tip: you can use Genozip to get coverage information, see: " WEBSITE_COVERAGE;

    if (E(BCF))
        valid_tips[n++] = "Tip: genozip compresses VCF files 5-10X faster than it compresses BCF files";
    
    if (E(SAM) || E(BAM))
        valid_tips[n++] = "Please take a moment now to make a note to not forget to cite Genozip:\n" PAPER3_CITATION "\n";

    bool is_deep = flag.deep || ((IS_PIZ && z_file && (Z_DT(SAM) || Z_DT(BAM))) && z_file->z_flags.dts2_deep);
    
    if (is_deep)
        valid_tips[n++] = "Please take a moment now to make a note to not forget to cite Genozip:\n" PAPER4_CITATION "\n";

    if (!is_deep && (E(SAM) || E(BAM) || E(FASTQ)) && sam_get_deep_tip()) 
        for (int i=0; i < 5; i++) // 5X more likely
            valid_tips[n++] = sam_get_deep_tip();

    if (E(VCF) || E(BCF))
        valid_tips[n++] = "Please take a moment now to make a note to not forget to cite Genozip:\n" PAPER1_CITATION "\n";

    if (!flag.optimize && (E(SAM) || E(BAM) || E(CRAM) || E(VCF) || E(BCF) || E(FASTQ))) 
        valid_tips[n++] = "Tip: using --optimize permits Genozip to make minor modifications to the data that usually have no impact on downstream analysis, yet result in significantly better compression, see: " WEBSITE_GENOZIP;

    if (!flag.best && !flag.fast && !flag.low_memory && !flag.make_reference) 
        valid_tips[n++] = "Tip: to achieve the best compression, use --best";

    if (license_is_eval() && !flag.show_stats)
        valid_tips[n++] = "Tip: to see detailed compression statistics, use --stats";

    if (license_is_eval() || license_is_standard() || license_is_enterprise())
        valid_tips[n++] = "Tip: \"genozip --sendto\" lets your clients send you compressed files using your Genozip license. See: " WEBSITE_PREMIUM;

    if (arch_get_max_resident_set() > 100 GB || flag.is_windows || flag.is_wsl || flag.is_mac)
        valid_tips[n++] = "Tip: with --low-memory, genozip will consume considerably less RAM, at the expense of compression size and time";

    iprintf ("\n%s\n", valid_tips[clock() % n]); // "randomly" select one of the valid tips

    flag.no_tip = true;
    sam_destroy_deep_tip();
}

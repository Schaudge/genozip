// ------------------------------------------------------------------
//   help-text.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "vcf.h"

static const char *help_genozip[] = {
    "",
    "Compress genomics files. Genozip can compress any file, but is optimally designed to compress the following file types:",
    "   VCF/BCF, SAM/BAM/CRAM, FASTQ, FASTA, GVF and 23andMe",
    "",
    "Usage: genozip [options]... [files or urls]...",
    "",
    "One or more file names or URLs may be given, or if omitted, standard input is used instead",
    "",
    "Supported input file types, as recognized by their listed filename extension(s): ",
    "   FASTA:   fasta, fa, faa, ffn, fnn, fna (possibly .gz .bgz .bz2 .xz)",
    "   FASTQ:   fastq, fq (possibly .gz .bgz .bz2 .xz)",
    "   SAM:     sam (possibly .gz .bgz .bz2 .xz)",
    "   BAM:     bam",
    "   CRAM:    cram",
    "   VCF:     vcf (possibly .gz .bgz .bz2 .xz)",
    "   BCF:     bcf (possibly .gz .bgz)",
    "   GVF:     gvf (possibly .gz .bgz .bz2 .xz)",
    "   23andMe: genome*Full*.txt (possibly zip)",
    "   Generic: any other file (possibly .gz .bgz .bz2 .xz)",
    "",
    "Note: for comressing .bcf, .cram or .xz files requires bcftools, samtools or xz, respectively, to be installed, as does using --index",
    "",
    "Examples: genozip sample.bam",
    "          genozip sample.R1.fq.gz sample.R2.fq.gz --pair --reference hg19.ref.genozip -o sample.genozip"
    "          genozip --optimize -password 12345 ftp://ftp.ncbi.nlm.nih.gov/file2.vcf.gz",
    "",
    "See also: genounzip genocat genols",
    "",
    "Actions - use at most one of these actions:",
    "   -d --decompress   Same as running genounzip. For more details, run: genounzip --help",
    "",
    "   -l --list         Same as running genols. For more details, run: genols --help",
    "",
    "   -h --help         <topic> Show this help page. Optional <topic> can be:",
    "                     dev   - list of developer options",
    "                     input - list of possible arguments of --input",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
    "",    
    "Flags:",    
    "   -i --input        <data-type>. data-type is one of the supported input file types listed above, examples: bam vcf.gz fq.xz. See \"genozip --help=input\" for full list of accepted file types",
    "",
    "                     This flag should be used when redirecting input data with a < or |, or if the input file type cannot be determined by its file name",
    "",
    "   -f --force        Force overwrite of the output file, or force writing " GENOZIP_EXT " data to standard output",    
    "",
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "",
    "   -o --output       <output-filename>. This option can also be used to bind multiple input files into a single genozip file. The files can be later unbound with 'genounzip --unbind'. To bind files, they must be of the same type (VCF, SAM etc) and if they are VCF files, they must contain the same samples. genozip takes advantage of similarities between the input files so that the bound file is usually smaller than the combined size of individually compressed files",
    "",
    "      --best         Best compression, but slower than --fast mode. This is the default mode of genozip - this flag has no additional effect.",
    "",
    "   -F --fast         Fast compression, but lower compression ratio than --best. Files compressed with this option also uncompress faster. Compressing with this option also consumes less memory.",
    "",
    "   -p --password     <password>. Password-protected - encrypted with 256-bit AES",
    "",
    "   -m --md5          Calculate the MD5 digest of the original textual file (vcf, sam...) instead of Adler32. The MD5 is also viewable with genols.",
    "                     Note: for compressed files, e.g. myfile.vcf.gz or myfile.bam, the MD5 calculated is that of the original, uncompressed textual file - myfile.vcf or myfile.sam respectively.",
    "",
    "   -I --input-size   <file size in bytes> genozip configures its internal data structures to optimize execution speed based on the file size. When redirecting the input file with < or |, genozip cannot determine its size, and this might result in slower execution. This problem can be overcome by using this flag to inform genozip of the file size",    
    "",
    "   -q --quiet        Don't show the progress indicator or warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -t --test         After compressing normally, decompresss in memory (i.e. without writing the decompressed file to disk) - comparing the MD5 of the resulting textual (vcf, sam) decompressed file to that of the original textual file. This option also activates --md5",
    "",
    "   -@ --threads      <number>. Specify the maximum number of threads. By default, genozip uses all the threads it needs to maximize usage of all available cores",
    "",
    "   -B --vblock       <number between 1 and 2048>. Set the maximum size of data (in megabytes) of the source textual (VCF, SAM, FASTQ etc) data that can go into one vblock. By default, this is set to "TXT_DATA_PER_VB_DEFAULT" MB. Smaller values will result in faster subsetting with --regions and --grep, while larger values will result in better compression. Note that memory consumption of both genozip and genounzip is linear with the vblock value used for compression",
    "",
    "   -e --reference    <filename>.ref.genozip Use a reference file - this is a FASTA file genozipped with the --make-reference option. The same reference needs to be provided to genounzip or genocat.",    
    "                     While genozip is capabale of compressing without a reference, in the following cases providing a reference may result in better compression:",
    "                     1. FASTQ files",
    "                     2. SAM/BAM files",
    "                     3. VCF files with significant REFALT content (see \"% of zip\" in --show-stats)",
    "",
    "   -E --REFERENCE    <filename>.ref.genozip Similar to --reference, except genozip copies the reference (or part of it) to the output file, so there is no need to specify --reference in genounzip and genocat.",
    "                     Note on using with --password: the copy of the reference file stored in the compressed file is never encrypted",  
    "",
    "   --make-reference  Compresss a FASTA file to be used as a reference in --reference or --REFERENCE. Ignored for non-FASTA files",
    "",
    "   -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   -W --SHOW-STATS   Show more detailed stats",
    "",
    "   --register        Register (or re-register) a non-commericial license to use genozip",

#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you're concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "Optimizing:",    
    "   -9 --optimize     Modify the file in ways that are likely insignificant for analytical purposes, but significantly improve compression and somewhat improve the speed of genocat --regions. --optimize activates all these optimizations, or they can be activated individually. These optimizations are:",    
    "",
    "   VCF optimizations: ",
    "   --optimize-sort   - INFO subfields are sorted alphabetically.               Example: AN=21;AC=3 -> AC=3;AN=21",
    "   --optimize-PL     - PL data: Phred values of over 60 are changed to 60.     Example: '0,18,270' -> '0,18,60'",    
    "   --optimize-GL     - GL data: Numbers are rounded to 2 significant digits.   Example: '-2.61618,-0.447624,-0.193264' -> '-2.6,-0.45,-0.19'",    
    "   --optimize-GP     - GP data: Numbers are rounded to 2 significant digits, as with GL.",
    "   --optimize-VQSLOD - VQSLOD data: Number is rounded to 2 significant digits. Example: '-4.19494' -> '-4.2'",
    "",
    "   SAM optimizations: ",
    "   --optimize-QUAL   - The QUAL quality field and the secondary U2 quality field (if it exists), are modified to group quality scores into a smaller number of bins:",
    "                       Quality scores of 2-9 are changed to 6; 10-19->15 ; 20-24->22 ; 25-29->27 ..... 85-89->87 ; 90-92->91 ; 93 unchanged",
    "                       This assumes a standard Sanger format of Phred quality scores 0->93 encoded in ASCII 33->126",
    "                       Note: this follows Illumina's quality bins for values up to Phred 39, and extends with additional similar bins for values of 40 and above common in some non-Illumina technologies: https://sapac.illumina.com/content/dam/illumina-marketing/documents/products/technotes/technote_understanding_quality_scores.pdf",
    "                       Example: 'LSVIHINKHK' -> 'IIIIFIIIFI'",
    "   --optimize-ZM     - ZM:B:s data: negative Ion Torrent flow signal values are changed to zero, and positives are rounded to the nearest 10.",
    "                       Example: '-20,212,427' -> '0,210,430'",
    "",
    "   FASTQ optimizations: ",
    "   --optimize-DESC   - Replaces the description line with '@filename:read_number'",
    "                       Example: '@A00488:61:HMLGNDSXX:4:1101:1561:1000 2:N:0:CTGAAGCT+ATAGAGGC' -> '@sample.fq.gz:100' (100 is the read sequential number within this fastq file)",
    "   --optimize-QUAL   - The quality data is optimized as described for SAM above",
    "",
    "   GVF optimizations: ",
    "   --optimize-sort   - Attributes are sorted alphabetically.                   Example: Notes=hi;ID=rs12 -> ID=rs12;Notes=hi",
    "   --optimize-Vf     - Variant_freq data: Number is rounded to 2 significant digits. Example: '0.006351' -> '0.0064'",
    "",
    "                     Note: due to these data modifications, files compressed with --optimize are NOT identical to the original file after decompression. For this reason, it is not possible to use this option in combination with --test or --md5",    
    "",
    "FASTQ-specific options (ignored for other file types):",
    "   -2 --pair         Compress pairs of paired-end FASTQ files, resulting in compression ratios better than compressing the files individually. When using this option, every two consecutive files on the file list should be paired-end FASTQ files with an identical number of reads and consistent file names, and --reference or --REFERENCE must be specified. The resulting genozip file is a bound file. To display interleaved, use genocat --interleave, and to unbind the genozip file back to its original FASTQ files, use genounzip --unbind.",
    "",
#ifndef _WIN32
    "VCF-specific options (ignored for other file types):",
    "   --gtshark         Uses gtshark as the codec for compressing FORMAT/GT data. This options provides better compression than both genozip without this option, and native gtshark, for files with a large number of samples. It works best when combined with -B128.",
    "                     Note: For this to work, gtshark needs to be installed - it is a separate software package that is not affiliated with genozip in any way. It can be found here: https://github.com/refresh-bio/GTShark",
    "                     Note: gtshark also needs to be installed for decompressing files that were compressed with this option. ",
    "",
#endif
     "genozip is available for free for non-commercial use and some other limited use cases. See 'genozip -L for details'. Commercial use requires a commercial license",
};

static const char *help_genozip_developer[] = {
    "",
    "Options useful mostly for developers of genozip:",
    "",
    "Usage: as flags for genozip (Z), genounzip (U), genocat (C), genols (L)",
    "",
    "     C  --one-vb <vb>     Reconstruct data from a single VB",
    "",
    "   ZUCL --show-time=<res> Show what functions are consuming the most time. Optional <res> is one of the members of ProfilerRec defined in profiler.h such 'compressor_lzma' or a substring such as 'compressor_'",
    "",
    "   ZUCL --show-memory     Show what buffers are consuming the most memory",
    "",
    "   ZUC  -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   ZUC  -W --SHOW-STATS   Show more detailed stats",
    "",
    "   ZUC  --show-alleles    (VCF only) Output allele values to stdout. Each row corresponds to a row in the VCF file. Mixed-ploidy regions are padded, and 2-digit allele values are replaced by an ascii character",
    "",
    "   ZUC  --show-dict=<field>  Show dictionary fragments written for each vblock (works for genounzip too). With optional <field> (eg CHROM, RNAME, POS, AN etc), shows only that one field. When used with genocat, only the dict data is shown, not the file contents",
    "",
    "   ZUC  --show-b250=<field>  Show b250 sections content - each value shows the line (counting from 1) and the index into its dictionary (note: REF and ALT are compressed together as they are correlated). With optional <field> (eg CHROM, RNAME, POS, AN etc), shows only that one field. This also works with genounzip and genocat, but without the line numbers. When used with genocat, only the dict data is shown, not the file contents",
    "",
    "   ZUC  --dump-b250=<field>",
    "   ZUC  --dump-local=<field>  Dump the binary content of the b250 or local data of this field, exactly as they appear in the genozip format, to a file named \"<field>.b250\" or \"<field>.local\" - specify the field name as it appears in the \"Name\" column in --SHOW-STATS, for fields that have \"comp local\" data. When used with genocat, the dump file will be created, and file contents will not be shown",
    "",
    "   ZUC  --list-chroms     List the names of the chromosomes (or contigs) included in the file",
    "",
    "   ZUC  --dump-section    <section-type>. Dump the uncompressed, unencrypted contents of all sections of this type (as it appears in --show-gheaders, eg SEC_REFERENCE), to a files named \"<section-type>.<vb>.<dict_id>.[header|body]\" - When used with genocat, the dump file will be created, and file contents will not be shown",
    "",
    "   ZUC  --show-headers    <section-type>. Show all the sections headers, or those of a specific section type if the optional argument is provided",
    "",
    "   ZUC  --show-index      Show the content of the random access index (SEC_RANDOM_ACCESS section). When used with genocat, only the index is shown, not the file contents",
    "",
    "   ZUC  --show-reference  Show the ranges included the SEC_REFERENCE sections",    
    "",
    "   ZUC  --show-ref-seq    Show the reference sequences as stored in genozip file or a reference file. Combine with --regions to see specific regions (genocat only). Combine with --sequential to omit newlines. '-' appears in unset loci. When used with genocat, only this is shown, not the file contents",
    "                          Note: the sequence stored in a .ref.genozip is NOT 100% identical to the FASTA that was used to generated it",
    "",
    "   ZUC  --show-ref-index  Show the content of the random access index of the reference data (SEC_REF_RAND_ACC section). When used with genocat, only the index is shown, not the file contents",
    "",
    "   ZUC  --show-ref-hash   Show the details of the reference hash table (SEC_REF_HASH) sections. When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-ref-contigs Show the details of the reference contigs. When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-ref-alts   Show the details of the file contigs that are mapped to a different contig name in the reference (eg '22' -> 'chr22'). When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-txt-contigs (SAM, BAM) Show the details of the contigs appearing the file header (SQ lines). When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-gheader    Show the content of the genozip header (which also includes the list of all sections in the file). When used with genocat, only the genozip header is shown, not the file contents",
    "",
    "   ZUC  --show-vblocks    Show vblock headers as they are read / written",
    "",
    "   ZUC  --show-threads    Show thread dispatcher activity",
    "",
    "   Z    --show-hash       See raw numbers that feed into determining the size of the global hash tables (calculated after the completion of vblock_i=1)",
    "",
    "   ZUC  --show-aliases    See contents of SEC_DICT_ID_ALIASES section",
    "",
    "    UC  --show-containers Show flow of container reconstruction",
    "",
    "   Z    --seg-only        Debug segmenting - runs the segmenter, but doesn't compress and doesn't write the output",
    "",
    "   ZUC  --xthreads        Use only 1 threads for the main PIZ/ZIP dispatcher. This doesn't affect thread use of other dispatchers",
    "",
    "   ZUCL --debug-memory    Buffer allocations and destructions",
    "",
    "   ZUC  --debug-progress  See raw numbers that feed into the progress indicator",
    "",
    "   ZUC  --show-reference  Show the ranges included the SEC_REFERENCE sections",    
    "",
    "   UC   --show-is-set     <contig> Shows the contents of SEC_REF_IS_SET section for the given contig",    
    "",
    "   Z    --show-codec      Genozip tests for the best codec when it first encounters a new type of data. See the results",    
    "",
    "   ZUC  --show-bgzf       Show bgzf blocks as they are being compressed or decompressed",    
    "",
    "   ZUC  --show-digest     Show digest (MD5 or Adler32) updates",    
    "",
    "   ZUC  --show-mutex      <mutex-name> Shows locks and unlocks of all mutexes or a particular mutex",    
};

static const char *help_genounzip[] = {
    "",
    "Uncompress genomic files previously compressed with genozip",
    "",
    "Usage: genounzip [options]... [files]...",
    "",
    "One or more file names must be given"
    "",
    "Examples: genounzip file1.vcf.genozip file2.sam.genozip",
    "          genounzip file.vcf.genozip --output file.vcf.gz",
    "          genounzip bound.vcf.genozip --unbind",
    "",
    "See also: genozip genocat genols",
    "",
    "Options:",
    "   -x --index        Create an index file alongside the decompressed file. The index file is created using 'samtools index' for SAM/BAM files, 'samtools faidx' for FASTA/FASTQ files and 'bcftools index' for VCF files. Other file formats cannot be indexed",
    "",
    "   -c --stdout       Send output to standard output instead of a file",
    "",
    "   -z --bgzf         Compress the output to the BGZF format (.gz extension).",
    "                     Note: if the original file was BGZF-compressed (such as .bam, .fastq.gz, .vcf.gz etc) we attempt to re-compress to the same byte-level identical compressed format as the source, using libdeflate level 6. However, the BGZF-compression might be slighly different if the source file was compressed with a different compression library or options. The underlying genomic data is always 100% identical.",
    "                     Note: this option is implicit if --output specifies a filename ending with .gz or .bgz",
    "                     Note: this option is implicit if the original file was compressed with BGZF or GZIP, unless output is to stdout (i.e. the terminal or a pipe)",
    "",
    "      --plain        Outputs a plain (i.e. not BGZF-compressed) file - negates implicit --bgzf",
    "",
    "      --bam          (SAM and BAM only) Output as BAM",
    "                     Note: this option is implicit if --output specifies a filename ending with .bam",
    "",
    "      --sam          (SAM and BAM only) Output as SAM",
    "                     Note: this option is implicit if --output specifies a filename ending with .sam[.gz|.bgz]",
    "",
    "      --no-PG        (SAM and BAM only) When converting a file from SAM to BAM or vice versa, Genozip normally adds a @PG line in the header. With this option, it doesn't",
    "",
    "      --fastq        (SAM and BAM only) Output as FASTQ",
    "                     The alignments are outputed as FASTQ reads in the order they appear in the SAM/BAM file. Alignments with FLAG 16 (reverse complimented) have their SEQ reverse complimented and their QUAL reversed. Alignments with FLAG 4 (unmapped) or 256 (secondary) are dropped. Alignments with FLAG 64 (or 128) (the first (or last) segment in the template) have a '1' (or '2') added after the read name. Usually, if the original order of the SAM/BAM file has not been tampered with, this would result in a valid interleaved FASTQ file.",
    "                     Note: this option is implicit if --output specifies a filename ending with one of .fq .fastq .fq.gz .fastq.gz",
    "",
    "      --bcf          (VCF only) Output as BCF, using bcftools",
    "                     Note: this option is implicit if --output specifies a filename ending with .bcf",
    "                     Note: bcftools needs to be installed for this option to work",
    "",
    "   -f --force        Force overwrite of the output file",
    "",
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "",
    "   -u --unbind[=prefix] Split a bound file back to its original components. If the '--unbind=prefix' form is used, a prefix is added to each file component. A prefix may include a directory.",
    "",
    "   -o --output       <output-filename>. Output to this filename instead of the default one",
    "",
    "   -e --reference    <filename>.ref.genozip Load a reference file prior to decompressing. Required for files compressed with --reference",    
    "",
    "   -p --password     <password>. Provide password to access file(s) that were compressed with --password",
    "",
    "   -m --md5          Show the digest of the decompressed file - MD5 if the file was compressed with --md5 or --test and Adler32 if not.",
    "                     Note: for compressed files, e.g. myfile.vcf.gz, the digest calculated is that of the original, uncompressed file. ",
    "",
    "   -q --quiet        Don't show the progress indicator or warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -t --test         Decompress in memory (i.e. without writing the decompressed file to disk) and use the digest (MD5 or Adler32) to verify that the resulting decompressed file is identical to the original file.",
    "",
    "   -@ --threads      <number>. Specify the maximum number of threads. By default, genozip uses all the threads it needs to maximize usage of all available cores",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you are concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "   -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   -W --SHOW-STATS   Show more detailed stats",
    "",
    "   -h --help         <topic> Show this help page. Optional <topic> can be:",
    "                     dev - list of developer options",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
};

static const char *help_genocat[] = {
    "",
    "Print original genomic file(s) previously compressed with genozip",
    "",
    "Usage: genocat [options]... [files]...",
    "",
    "One or more file names must be given",
    "",
    "See also: genozip genounzip genols",
    "",
    "Reference-file related options:",    
    "   -e --reference    <filename>.ref.genozip Load a reference file prior to decompressing. Required for files compressed with --reference",    
    "",
    "                     When no non-reference file is specified, display the reference data itself. Typically used in combination with --regions",    
    "",
    "   -E --REFERENCE    <filename>.ref.genozip with no non-reference file specified. Display the reverse complement of the reference data itself. Typically used in combination with --regions",
    "",
    "   --show-reference  Show the name and MD5 of the reference file that needs to be provided to uncompress this file",    
    "",
    "Subsetting options (options resulting in modified display of the data):",    
    "   --downsample      <rate> Show only one in every <rate> lines (or reads in the case of FASTQ). Other subsetting options, if any, will be applied to the surviving lines only.",
    "",
    "   --interleave      For FASTQ data compressed with --pair: Show every pair of paired-end FASTQ files with their reads interleaved: first one read of the first file, then a read from the second file, then the next read from the first file and so on.",
    "",
    "   -r --regions      [^]chr|chr:pos|pos|chr:from-to|chr:from-|chr:-to|from-to|from-|-to|from+len[,...]",
    "   VCF SAM FASTA     Show one or more regions of the file. Examples:",
    "   GVF 23andMe                 genocat myfile.vcf.genozip -r22:1000-2000  (Positions 1000 to 2000 on contig 22)",
    "   ref                         genocat myfile.sam.genozip -r22:1000+151   (151 bases, starting pos 1000, on contig 22)",
    "                               genocat myfile.vcf.genozip -r-2000,2500-   (Two ranges on all contigs)",
    "                               genocat myfile.sam.genozip -rchr21,chr22   (Contigs chr21 and chr22 in their entirety)",            
    "                               genocat myfile.vcf.genozip -r^MT,Y         (All contigs, excluding MT and Y)",            
    "                               genocat myfile.vcf.genozip -r^-1000        (All contigs, excluding positions up to 1000)",            
    "                               genocat myfile.fa.genozip  -rchrM          (Contig chrM)",            
    "                     Note: genozip files are indexed automatically during compression. There is no separate indexing step or separate index file",            
    "                     Note: Indels are considered part of a region if their start position is",
    "                     Note: Multiple -r arguments may be specified - this is equivalent to chaining their regions with a comma separator in a single argument",
    "                     Note: For FASTA files, only whole-contig regions are possible",
    "",
    "   -s --samples      [^]sample[,...]",
    "   VCF               Show a subset of samples (individuals). Examples:",
    "                               genocat myfile.vcf.genozip -s HG00255,HG00256    (show two samples)",
    "                               genocat myfile.vcf.genozip -s ^HG00255,HG00256   (show all samples except these two)",
    "                     Note: This does not change the INFO data (including the AC and AN tags)",
    "                     Note: sample names are case-sensitive",
    "                     Note: Multiple -s arguments may be specified - this is equivalent to chaining their samples with a comma separator in a single argument",
    "",
    "   -g --grep         <string> Show only records in which <string> is a case-sensitive substring of the description",
    "   FASTQ FASTA",
    "",
    "   --list-chroms     List the names of the chromosomes (or contigs) included in the file",
    "   VCF SAM FASTA GVF 23andMe ",
    "",
    "   -G --drop-genotypes Output the data without the samples and FORMAT column",
    "   VCF",
    "",
    "   -H --no-header    Don't output the header lines",
    "",
    "   -1 --header-one   VCF: Output only the last line on the header (the line with the field and sample names)",
    "   VCF FASTA         FASTA: Output the sequence name up to the first space or tab",
    "",
    "   --header-only     Output only the header lines",
    "",
    "   --GT-only         For samples, output only genotype (GT) data, dropping the other subfields",
    "   VCF",
    "",
    "   --sequential      Output in sequential format - each sequence in a single line",
    "   FASTA",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you're concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "Translation options (options resulting convertion of the data from one format to another):",    
    "      --bam          (SAM and BAM only) Output as BAM.",
    "                     Note: this option is implicit if --output specifies a filename ending with .bam",
    "",
    "      --sam          (SAM and BAM only) Output as SAM. This option is the default in genocat.",
    "",
    "      --fastq        (SAM and BAM only) Output as FASTQ",
    "                     The alignments are outputed as FASTQ reads in the order they appear in the SAM/BAM file. Alignments with FLAG 16 (reverse complimented) have their SEQ reverse complimented and their QUAL reversed. Alignments with FLAG 4 (unmapped) or 256 (secondary) are dropped. Alignments with FLAG 64 (or 128) (the first (or last) segment in the template) have a '1' (or '2') added after the read name. Usually, if the original order of the SAM/BAM file has not been tampered with, this would result in a valid interleaved FASTQ file.",
    "                     Note: this option is implicit if --output specifies a filename ending with one of .fq .fastq .fq.gz .fastq.gz",
    "",
    "      --bcf          (VCF only) Output as BCF, using bcftools",
    "                     Note: bcftools needs to be installed for this option to work",
    "",
    "      --phylip       (FASTA only) Output a Multi-FASTA in Phylip format. All sequences must be the same length",
    "",
    "      --fasta        (Phylip only) Output as Multi-FASTA",
    "",
    "      --vcf          (23andMe only) Output as VCF. --vcf must be used in combination with --reference to specify the reference file as listed in the header of the 23andMe file (usually this is GRCh37)",
    "                     Note: INDEL genotypes ('DD', 'DI', 'II') as well as uncalled sites ('--') are discarded",
    "",
    "General options:",
    "   -o --output       <output -filename>. Output to this filename instead of stdout",
    "",
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "",
    "   -@ --threads      Specify the maximum number of threads. By default, genozip uses all the threads it needs to maximize usage of all available cores",
    "",
    "   -x --index        Create an index file alongside the decompressed file, when combined with --output. The index file is created using 'samtools index' for SAM/BAM files, 'samtools faidx' for FASTA/FASTQ files and 'bcftools index' for VCF files. Other file formats cannot be indexed",
    "",
    "   -q --quiet        Don't show warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   -W --SHOW-STATS   Show more detailed stats",
    "",
    "   -h --help         <topic> Show this help page. Optional <topic> can be:",
    "                     dev - list of developer options",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
    "",
    "Tip regarding using genozip files in a pipeline:",
    "",
    "   Option 1: For tools that support input redirection - use a regular pipe. Example:",
    "             genocat myfile.vcf.genozip | bcftools view -",
    "",
    "   Option 2: For tools that don't support input redirection - use a named pipe. Example:",
    "             mkfifo mypipe.vcf",
    "             genocat myfile.vcf.genozip > mypipe.vcf &",
    "             othertool mypipe.vcf",
};

static const char *help_genols[] = {
    "",
    "View metadata of genomic files previously compressed with genozip",
    "",
    "Usage: genols [options]... [files or directories]...",
    "",
    "One or more file or directory names may be given, or if omitted, genols runs on the current directory",
    "",
    "See also: genozip genounzip genocat",
    "",
    "Options:",
    "   -u --unbind       Show the components of bound files. This option is implied when running genols on a single file",
    "",
    "   -b --bytes        Show sizes in bytes",    
    "",
    "   -q --quiet        Don't show warnings",    
    "",
    "   -h --help         Show this help page",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
};

static const char *help_footer[] = {
    "",
    "Citing: Lan, D., et al. Bioinformatics, Volume 36, Issue 13, July 2020, Pages 4091-4092",
    "",
    "Bug reports and feature requests: bugs@genozip.com",
    "Commercial license inquiries: sales@genozip.com",
    "Requests for support for compression of additional public or proprietary genomic file formats: sales@genozip.com",
    "",
    "THIS SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    ""
};


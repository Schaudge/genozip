// ------------------------------------------------------------------
//   help-text.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "vcf.h"

static const char *help_genozip[] = {
    "",
    "Compress genomics files. Currently supported file types: VCF/BCF, SAM/BAM/CRAM, FASTQ, FASTA, GVF and 23andMe",
    "",
    "Usage: genozip [options]... [files or urls]...",
    "",
    "One or more file names or URLs may be given, or if omitted, standard input is used instead",
    "",
    "Supported input file types: ",
    "   VCF: vcf (possibly .gz .bgz .bz2 .xz), bcf (possibly .gz .bgz)",
    "   SAM: sam (possibly .gz .bgz .bz2 .xz), bam, cram",
    "   FASTQ: fastq, fq (possibly .gz .bgz .bz2 .xz)",
    "   FASTA: fasta, fa, faa, ffn, fnn, fna (possibly .gz .bgz .bz2 .xz)",
    "   GVF: gvf (possibly .gz .bgz .bz2 .xz)",
    "   23andMe: genome*Full*.txt (possibly zip)",
    "",
    "Note: for comressing .bcf, .bam or .xz files requires bcftools, samtools or xz, respectively, to be installed",
    "",
    "Examples: genozip file1.vcf file2.vcf -o bound.vcf.genozip",
    "          genozip --optimize -password 12345 ftp://ftp.ncbi.nlm.nih.gov/file2.vcf.gz",
    "",
    "See also: genounzip genocat genols",
    "",
    "Actions - use at most one of these actions:",
    "   -d --decompress   Same as running genounzip. For more details, run: genounzip --help",
    "",
    "   -l --list         Same as running genols. For more details, run: genols --help",
    "",
    "   -h --help         Show this help page. Use with -f to see developer options.",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
    "",    
    "Flags:",    
    "   -i --input-type   <data-type>. data-type is one of the supported input file types listed above, examples: bam vcf.gz fq.xz",
    "                     \"genozip -i help\" for full list of accepted file types"
    "",
    "                     This flag should be used when redirecting input data with a < or |, or if the input file type cannot be determined by its file name",
    "",
    "   -c --stdout       Send output to standard output instead of a file",    
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
    "   -m --md5          Calculate the MD5 hash of the original textual file (vcf, sam...). When the resulting file is decompressed, this MD5 will be compared to the MD5 of the textual decompressed file. This also enables using 'genounzip --test' and viewing the MD5 with genols.",
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
    "   -2 --pair         Compress pairs of paired-end consecutive files, resulting in compression ratios better than compressing the files individually. When using this option, every two consecutive files on the file list should be paired read fastq files with an identical number of reads, --reference or --REFERENCE must be specified. The resulting genozip file is a bound file. To unbind the genozip file back to its original FASTQ files, use genounzip --unbind",
    "",
    "genozip is available for free for non-commercial use and some other limited use cases. See 'genozip -L for details'. Commercial use requires a commercial license",
};

static const char *help_genozip_developer[] = {
    "",
    "Options useful mostly for developers of genozip:",
    "",
    "Usage: as flags for genozip (Z), genounzip (U), genocat (C), genols (L)",
    "",
    "   ZUCL --show-time       Show what functions are consuming the most time",
    "",
    "   ZUCL --show-memory     Show what buffers are consuming the most memory",
    "",
    "   ZUC  -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   ZUC  -W --SHOW-STATS   Show more detailed stats",
    "",
    "   ZUC  --show-alleles    (VCF only) Output allele values to stdout. Each row corresponds to a row in the VCF file. Mixed-ploidy regions are padded, and 2-digit allele values are replaced by an ascii character",
    "",
    "   ZUC  --show-dict       Show dictionary fragments written for each vblock (works for genounzip too). When used with genocat, only the dict data is shown, not the file contents",
    "",
    "   ZUC  --show-one-dict   <field-name>. Show the dictionary for this field in a tab-separated list - <field-name> may be one of the fields 1-9 (CHROM to FORMAT) or a INFO tag or a FORMAT tag (works for genounzip too). When used with genocat, only the index is shown, not the file contents",
    "",
    "   ZUC  --list-chroms     List the names of the chromosomes (or contigs) included in the file",
    "",
    "   ZUC  --show-b250       Show b250 sections content - each value shows the line (counting from 1) and the index into its dictionary (note: REF and ALT are compressed together as they are correlated). This also works with genounzip and genocat, but without the line numbers. When used with genocat, only the dict data is shown, not the file contents",
    "",
    "   ZU   --show-one-b250   <field-name>. Show the values for this field or subfield - can be a field like CHROM, RNAME or a subfield like DP",
    "",
    "   ZUC  --dump-one-b250   <field-name>. Dump the binary content of the b250 data of this field, exactly as they appear in the genozip format, to a file named \"<field-name>.b250\" - specify the field name as it appears in the \"Name\" column in --show-stats, for fields that have \"comp b250\" data. When used with genocat, the dump file will be created, and file contents will not be shown",
    "",
    "   ZUC  --dump-one-local  <field-name>. Dump the binary content of the local data of this field, exactly as they appear in the genozip format, to a file named \"<field-name>.local\" - specify the field name as it appears in the \"Name\" column in --show-stats, for fields that have \"comp local\" data. When used with genocat, the dump file will be created, and file contents will not be shown",
    "",
    "   ZUC  --show-headers    Show the sections headers",
    "",
    "   ZUC  --show-index      Show the content of the random access index (SEC_RANDOM_ACCESS section). When used with genocat, only the index is shown, not the file contents",
    "",
    "   ZUC  --show-reference  Show the ranges included the SEC_REFERENCE sections",    
    "",
    "   ZUC  --show-ref-seq    Show the reference sequences. Combine with --regions to see specific regions (genocat only). Combine with --sequential to omit newlines. '-' appears in unset loci. When used with genocat, only this is shown, not the file contents",
    "",
    "   ZUC  --show-ref-index  Show the content of the random access index of the reference data (SEC_REF_RAND_ACC section). When used with genocat, only the index is shown, not the file contents",
    "",
    "   ZUC  --show-ref-hash   Show the details of the reference hash table (SEC_REF_HASH) sections. When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-ref-contigs Show the details of the reference contigs. When used with genocat, only this data is shown, not the file contents",
    "",
    "   ZUC  --show-ref-alts   Show the details of the file contigs that are mapped to a different contig name in the reference (eg '22' -> 'chr22'). When used with genocat, only this data is shown, not the file contents",
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
    "   Z    --test-seg        Debug segmenting - runs the segmenter, but doesn't compress and doesn't write the output",
    "",
    "   ZUCL --debug-memory    Buffer allocations and destructions",
    "",
    "   ZUC  --debug-progress  See raw numbers that feed into the progress indicator",
    "",
    "   ZUC  --show-reference  Show the ranges included the SEC_REFERENCE sections",    
    "",
    "   UC   --show-is-set     <contig> Shows the contents of SEC_REF_IS_SET section for the given contig",    
    "",
    "   Z    --show-codec-test Genozip tests for the best codec when it first encounters a new type of data. See the results",    
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
    "   -c --stdout       Send output to standard output instead of a file",
    "",
    "   -z --bgzip        Compress the output VCF, FASTA, FASTQ or GVF file(s) with bgzip",
    "                     Note: this option is implicit if --output specifies a filename ending with .vcf.gz or .vcf.bgz",
    "                     Note: bgzip needs to be installed for this option to work",
    "",
    "      --bcf          Compress the output VCF file(s) with bcftools",
    "                     Note: this option is implicit if --output specifies a filename ending with .bcf",
    "                     Note: bcftools needs to be installed for this option to work",
    "",
    "      --bam          Compress the output SAM file(s) with samtools",
    "                     Note: this option is implicit if --output specifies a filename ending with .bam",
    "                     Note: samtools needs to be installed for this option to work",
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
    "   -m --md5          Show the MD5 hash of the decompressed VCF file. If the file was originally compressed with --md5, it also verifies that the MD5 of the original VCF file is identical to the MD5 of the decompressed VCF.",
    "                     Note: for compressed files, e.g. myfile.vcf.gz, the MD5 calculated is that of the original, uncompressed file. ",
    "",
    "   -q --quiet        Don't show the progress indicator or warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -t --test         Decompress in memory (i.e. without writing the decompressed file to disk) - comparing the MD5 of the resulting decompressed file to that of the original VCF. Works only if the file was compressed with --md5",
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
    "   -h --help         Show this help page. Use with -f to see developer options.",
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
    "Options:",    
    "   -e --reference    <filename>.ref.genozip Load a reference file prior to decompressing. Required for files compressed with --reference",    
    "",
    "                     When no non-reference file is specified, display the reference data itself. Typically used in combination with --regions",    
    "",
    "   -E --REFERENCE    <filename>.ref.genozip with no non-reference file specified. Display the reverse complement of the reference data itself. Typically used in combination with --regions",
    "",
    "   --show-reference  Show the name and MD5 of the reference file that needs to be provided to uncompress this file",    
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
    "   -1 --header-one   Don't output the VCF header, except for the last line (with the field and sample names)",
    "   VCF",
    "",
    "   --header-only     Output only the header lines",
    "",
    "   --GT-only         For samples, output only genotype (GT) data, dropping the other subfields",
    "   VCF",
    "",
    "   --sequential      Output in sequential format - each sequence in a single line",
    "   FASTA",
    "",
    "   -o --output       <output -filename>. Output to this filename instead of stdout",
    "",
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "",
    "   -@ --threads      Specify the maximum number of threads. By default, genozip uses all the threads it needs to maximize usage of all available cores",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you're concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "   -q --quiet        Don't show warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -w --show-stats   Show the internal structure of a genozip file and the associated compression stats",
    "",
    "   -W --SHOW-STATS   Show more detailed stats",
    "",
    "   -h --help         Show this help page. Use with -f to see developer options. Use --header-only if that is what you are looking for",
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


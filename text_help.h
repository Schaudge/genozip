// ------------------------------------------------------------------
//   help-text.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "zip.h"
#include "gtshark.h"

static const char *help_genozip[] = {
    "",
    "Compress VCF (Variant Call Format) files",
    "",
    "Usage: genozip [options]... [files or urls]...",
    "",
    "One or more file names or URLs may be given, or if omitted, standard input is used instead",
    "",
    "Supported input file types: .vcf .vcf.gz .vcf.bgz .vcf.bz2 .vcf.xz .bcf .bcf.gz .bcf.bgz",
    "Note: for .bcf files, bcftools needs to be installed, and for .xz files, xz needs to be installed",
    "",
    "Examples: genozip file1.vcf file2.vcf -o concat.vcf.genozip",
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
    "   -c --stdout       Send output to standard output instead of a file",    
    "",
    "   -f --force        Force overwrite of the output file, or force writing .vcf" GENOZIP_EXT " data to standard output",    
    "",
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "",
    "   -o --output       <output-filename>. This option can also be used to concatenate multiple input files with the same individuals, into a single concatenated output file",
    "",
    "   -p --password     <password>. Password-protected - encrypted with 256-bit AES",
    "",
    "   -m --md5          Calculate the MD5 hash of the VCF file. When the resulting file is decompressed, this MD5 will be compared to the MD5 of the decompressed VCF.",
    "                     Note: for compressed files, e.g. myfile.vcf.gz, the MD5 calculated is that of the original, uncompressed file. ",
    "                     In addition, if the VCF file has Windows-style \\r\\n line endings, the md5 will be that of the modified file with the \\r removed",
    "",
    "   -q --quiet        Don't show the progress indicator or warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -t --test         After compressing normally, decompresss in memory (i.e. without writing the decompressed file to disk) - comparing the MD5 of the resulting decompressed file to that of the original VCF. This option also activates --md5",
    "",
    "   -@ --threads      <number>. Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O.",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you're concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "   --show-content    Show the information content of VCF files and the compression ratios of each component",
    "",
    "Optimizing:",    
    "   -9 --optimize     Modify the VCF file in ways that are likely insignificant for analytical purposes, but make a significant difference for compression. At the moment, these optimizations include:",    
    "                     - PL data: Phred values of over 60 are changed to 60.     Example: '0,18,270' -> '0,18,60'",    
    "                     - GL data: Numbers are rounded to 2 significant digits.   Example: '-2.61618,-0.447624,-0.193264' -> '-2.6,-0.45,-0.19'",    
    "                     - GP data: Numbers are rounded to 2 significant digits, as with GL.",
    "                     - VQSLOD data: Number is rounded to 2 significant digits. Example: '-4.19494' -> '-4.2'",
    "                     Note: due to these data modifications, files compressed with --optimized are NOT identical as the original VCF after decompression. For this reason, it is not possible to use this option in combination with --test or --md5",    
    "",
    "   -B --vblock       <number between 1 and 2048>. Sets the maximum size of memory (in megabytes) of VCF file data that can go into one variant block. By default, this is set to "VCF_DATA_PER_VB" MB. The variant block is the basic unit of data on which genozip and genounzip operate. This value affects a number of things: 1. Memory consumption of both compression and decompression are linear with the variant block size. 2. Compression is sometimes better with larger block sizes, in particular if the number of samples is small. 3. Smaller blocks will result in faster 'genocat --regions' lookups",    
    "",
    "   -S --sblock       <number>. Sets the number of samples per sample block. By default, it is set to "SAMPLES_PER_BLOCK". When compressing or decompressing a variant block, the samples within the block are divided to sample blocks which are compressed separately. A higher value will result in a better compression ratio, while a lower value will result in faster 'genocat --samples' lookups",
    "",
    "   -K --gtshark      Use gtshark instead of the default bzlib as the final compression step for allele data (the GT subfield in the sample data). ",
    "                     Note: For this to work, gtshark needs to be installed - it is a separate software package that is not affliated with genozip in any way. It can be found here: https://github.com/refresh-bio/GTShark",
    "                     Note: gtshark also needs to be installed for decompressing files that were compressed with this option. ",
    "                     Note: This option isn't supported on Windows",
    "",
    "genozip is available for free for non-commercial use and some other limited use cases. See 'genozip -L for details'. Commercial use requires a commercial license",
};

static const char *help_genozip_developer[] = {
    "",
    "Options useful mostly for developers of genozip:",
    "",
    "Usage: as flags for genozip, genounzip, genocat, genols (not all flags are suported on all tools)",
    "",
    "   --show-time       Show what functions are consuming the most time",
    "",
    "   --show-memory     Show what buffers are consuming the most memory",
    "",
    "   --show-sections   Show the section types of the output genozip file and the compression ratios of each component",
    "",
    "   --show-alleles    Output allele values to stdout. Each row corresponds to a row in the VCF file. Mixed-ploidy regions are padded, and 2-digit allele values are replaced by an ascii character",
    "",
    "   --show-dict       Show dictionary fragments written for each variant block (works for genounzip too)",
    "",
    "   --show-one-dict   <field-name> Show the dictionary for this field in a tab-separated list - <field-name> may be one of the fields 1-9 (CHROM to FORMAT) or a INFO tag or a FORMAT tag (works for genounzip too)",
    "",
    "   --show-gt-nodes   Show transposed GT matrix - each value is an index into its dictionary",
    "",
    "   --show-b250       Show fields 1-9 (CHROM to FORMAT) as well as INFO tags - each value shows the line (counting from 1) and the index into its dictionary (note: REF and ALT are compressed together as they are correlated). This also works with genounzip, but without the line numbers.",
    "",
    "   --show-one-b250   <field-name> Show the values for this field - may be one of the fields 1-9 (CHROM to FORMAT) or an INFO tag",
    "",
    "   --dump-one-b250   <field-name> Dump the binary content of this field, exactly as they appear in the genozip format, to stdout - may be one of the fields 1-9 (CHROM to FORMAT) or an INFO tag",
    "",
    "   --show-headers    Show the sections headers (works for genounzip too)",
    "",
    "   --show-index      Show the content of the random access index",
    "",
    "   --show-gheader    Show the content of the genozip header (which also includes the list of all sections in the file)",
    "",
    "   --show-threads    Show thread dispatcher activity",
    "",
    "   --debug-memory    Buffer allocations and destructions",
};

static const char *help_genounzip[] = {
    "",
    "Uncompress VCF (Variant Call Format) files previously compressed with genozip",
    "",
    "Usage: genounzip [options]... [files]...",
    "",
    "One or more file names must be given"
    "",
    "Examples: genounzip file1.vcf.genozip file2.vcf.genozip",
    "          genounzip file.vcf.genozip --output file.vcf.gz",
    "          genounzip concat.vcf.genozip --split",
    "",
    "See also: genozip genocat genols",
    "",
    "Options:",
    "   -c --stdout       Send output to standard output instead of a file",
    "",
    "   -z --bgzip        Compress the output VCF file(s) with bgzip",
    "                     Note: this option is implicit if --output specifies a filename ending with .gz or .bgz",
    "                     Note: bgzip needs to be installed for this option to work",
    "",
    "   -f --force        Force overwrite of the output file",
    "",
    "   -^ --replace      Replace the source file with the result file, rather than leaving it unchanged",    
    "",
    "   -O --split        Split a concatenated file back to its original components",
    "",
    "   -o --output       <output-filename>. Output to this filename instead of the default one",
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
    "   -@ --threads      <number>. Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you are concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "   -h --help         Show this help page. Use with -f to see developer options.",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
};

static const char *help_genols[] = {
    "",
    "View metadata of VCF (Variant Call Format) files previously compressed with genozip",
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

static const char *help_genocat[] = {
    "",
    "Print VCF (Variant Call Format) file(s) previously compressed with genozip",
    "",
    "Usage: genocat [options]... [files]...",
    "",
    "One or more file names must be given",
    "",
    "See also: genozip genounzip genols",
    "",
    "Options:",    
    "   -r --regions      [^]chr|chr:pos|pos|chr:from-to|chr:from-|chr:-to|from-to|from-|-to[,...]",
    "                     Show one or more regions of the file. Examples:",
    "                               genocat myfile.vcf.genozip -r22:1000000-2000000  (A range of chromosome 22)",
    "                               genocat myfile.vcf.genozip -r-2000000,2500000-   (Two ranges of all chromosomes)",
    "                               genocat myfile.vcf.genozip -r21,22               (All of chromosome 21 and 22)",            
    "                               genocat myfile.vcf.genozip -r^MT,Y               (All of chromosomes except for MT and Y)",            
    "                               genocat myfile.vcf.genozip -r^-10000             (All sites on all chromosomes, except positions up to 10000)",            
    "                     Note: genozip files are indexed automatically during compression. There is no separate indexing step or separate index file",            
    "                     Note: Indels are considered part of a region if their start position is",
    "                     Note: Multiple -r arguments may be specified - this is equivalent to chaining their regions with a comma separator in a single argument",
    "",
    "   -t --targets      Identical to --regions, provided for pipeline compatibility",
    "",
    "   -s --samples      [^]sample[,...]",
    "                     Show a subset of samples (individuals). Examples:",
    "                               genocat myfile.vcf.genozip -s HG00255,HG00256    (show two samples)",
    "                               genocat myfile.vcf.genozip -s ^HG00255,HG00256   (show all samples except these two)",
    "                     Note: This does not change the INFO data (including the AC and AN tags)",
    "                     Note: sample names are case-sensitive",
    "                     Note: Multiple -s arguments may be specified - this is equivalent to chaining their samples with a comma separator in a single argument",
    "",
    "   -G --drop-genotypes Output the data without the individual genotypes and FORMAT column",
    "",
    "   -H --no-header    Don't output the VCF header",
    "",
    "      --header-only  Output only the VCF header",
    "",
    "      --GT-only      For samples, output only genotype (GT) data, dropping the other subfields",
    "",
    "      --strip        Don't output values for ID, QUAL, FILTER, INFO; FORMAT is only GT (at most); Samples include allele values (i.e. GT subfield) only",
    "",
    "   -o --output       <output-filename>. Output to this filename instead of stdout",
    "",
    "   -p --password     Provide password to access file(s) that were compressed with --password",
    "",
    "   -@ --threads      Specify the maximum number of threads. By default, this is set to the number of cores available. The number of threads actually used may be less, if sufficient to balance CPU and I/O",
#if !defined _WIN32 && !defined __APPLE__ // not relevant for personal computers
    "                     Tip: if you're concerned about sharing the computer with other users, rather than using --threads to reduce the number of threads, a better option would be to use the command nice, e.g. 'nice genozip....'. This yields CPU to other users if needed, but still uses all the cores that are available",
#endif
    "",
    "   -q --quiet        Don't show warnings",    
    "",
    "   -Q --noisy        The --quiet is turned on by default when outputting to the terminal. --noisy stops the suppression of warnings",    
    "",
    "   -h --help         Show this help page. Use with -f to see developer options. Use --header-only if that is what you are looking for",
    "",
    "   -L --license      Show the license terms and conditions for this product",
    "",
    "   -V --version      Display version number",
};

static const char *help_footer[] = {
    "",
    "Bug reports and feature requests: bugs@genozip.com",
    "Commercial license inquiries: sales@genozip.com",
    "",
    "THIS SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
    ""
};


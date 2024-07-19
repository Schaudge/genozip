#!/usr/bin/env bash
# ^ finds bash according to $PATH

# ------------------------------------------------------------------
#   test.sh
#   Copyright (C) 2019-2024 Genozip Limited. Patent Pending.
#   Please see terms and conditions in the file LICENSE.txt

cleanup_cache()
{
    $genozip --no-cache
}

install_license()
{
    $SCRIPTSDIR/install_license.sh $1 || exit 1
}

cleanup() 
{ 
    rm -fR $OUTDIR/* $TESTDIR/*.bad $TESTDIR/*.rejects.* 
    cleanup_cache
    install_license Premium || exit 1
    unset GENOZIP_REFERENCE    
}

# compares two files using internal MD%, allowing each file to be gz-compressed or not
cmp_2_files() 
{
    if [ ! -f $1 ] ; then echo "File $1 not found while in cmp_2_files()"; exit 1; fi
    if [ ! -f $2 ] ; then echo "File $2 not found while in cmp_2_files()"; exit 1; fi

    if [[ `$zmd5 $1` != `$zmd5 $2` ]] ; then
        echo "MD5 comparison FAILED: $1 $2"
        echo `$zmd5 "$1"` "$1" 
        echo `$zmd5 "$2"` "$2"
        exit 1
    fi
}

# compares two files using external MD5, requiring that they have the same gz compression
cmp_2_files_exact() 
{
    if [ ! -f $1 ] ; then echo "File $1 not found while in cmp_2_files()"; exit 1; fi
    if [ ! -f $2 ] ; then echo "File $2 not found while in cmp_2_files()"; exit 1; fi

    if [[ "`$md5 $1 | cut -d' ' -f1`" != "`$md5 $2 | cut -d' ' -f1`" ]] ; then
        echo "MD5 comparison FAILED: $1 $2"
        echo `$md5 "$1"`
        echo `$md5 "$2"`
        exit 1
    fi
}

verify_failure() # $1=exe $2=$? 
{
    if (( $2 == 0 )); then echo "Error: expecting $1 to fail but it succeeded"; exit 1; fi
}

test_header() 
{
    sep="=======================================================================================================\n"
    printf "\n${sep}TESTING ${FUNCNAME[2]} (batch_id=${GENOZIP_TEST}): "
    echo $1 | tr "\\\\" "/" # \ -> \\ coming from $path string on Windows
    printf "$sep"
}

test_count_genocat_lines() 
{ # $1 - genozip arguments $2 - genocat arguments $3 - expected number of output lines
    test_header "genozip $1 ; genocat $2"
    
    if [ ! -z "$1" ]; then
        $genozip $1 -Xfo $output || exit 1
    fi
    
    $genocat $output $2 -fo $recon || exit 1
    local wc=`cat $recon |wc -l`

    if (( $wc != $3 )); then
        echo "FAILED - expected $3 lines, but getting $wc"
        exit 1
    fi  
}

test_standard()  # $1 genozip args $2 genounzip args $3... filenames 
{
    local zip_args=( $1 )
    local unzip_args=( $2 )
    local args=( "$@" )

    local copies=()
    if [[ ${zip_args[0]} == "COPY" ]]; then
        zip_args=( ${zip_args[@]:1} ) # remove COPY
        for file in ${args[@]:2}; do 
            cp $TESTDIR/$file $OUTDIR/copy.$file
            copies+=( $OUTDIR/copy.$file )
            args+=( tmp/copy.$file ) # adding the ${TESTDIR}/ prefix in a sec
        done
    fi

    local files=( ${args[@]:2} )
    if [[ ${zip_args[0]} == "NOPREFIX" ]]; then
        zip_args=( ${zip_args[@]:1} ) # remove NOPREFIX
    else
        files=( "${files[@]/#/${TESTDIR}/}" )
    fi

    local single_output=0
    if [[ ${zip_args[0]} == "CONCAT" ]]; then
        zip_args=( ${zip_args[@]:1} ) # remove CONCAT
        local single_output=1
    fi

    test_header "$genozip ${zip_args[*]} ${files[*]}"    # after COPY, NOPREFIX and CONCAT modifications occurred
    
    if (( ${#files[@]} == 1 || $single_output )); then   # test with Adler32, unless caller specifies --md5
        $genozip -X ${zip_args[@]} ${files[@]} -o $output -f || exit 1
        $genounzip ${unzip_args[@]} $output -t || exit 1
    elif cmp --silent `echo "$genozip" | cut -d" " -f1` `echo "$genounzip" | cut -d" " -f1`; then # --test will work correctly only if genozip==genounzip (otherwise we won't be testing the intended PIZ executable)
        $genozip ${zip_args[@]} ${unzip_args[@]} ${files[@]} -ft || exit 1
    else
        echo "skipping test because genozip and genounzip are different executables" 
    fi
    
    if [ ! -n "$single_output" ]; then
        local count=`ls -1 $TESTDIR/*.genozip | wc -l`   # unfortunately, these go to TESTDIR not OUTDIR
        local num_files=$(( $# - 1 ))
        if (( $count != $num_files )); then
            echo "Error: compressed $num_files files, but only $count genozip files found. Files compressed: "
            echo ${files[@]}
            exit 1
        fi
        cleanup
    fi
}

test_redirected() { # $1=filename  $2...$N=optional extra genozip arg
    test_header "$1 - redirecting $file to genozip via stdin"
    local file=$TESTDIR/$1
    local args=( "$@" )

    # instead of passing the input, we pass the filename. that works, because the code compares extensions
    local ext=${file##*.}
    if [[ $ext == 'gz' || $ext == 'bz2' || $ext == 'xz' || $ext == 'zip' ]]; then
        local f=${file##*.} # remove .gz etc
        local ext=${f##*.}
    fi

    # file name extension of basic.* has is_data_type callback defined in DATA_TYPE_PROPERTIES
    local has_is_data_type=( vcf sam fastq fq fa gff gvf gtf bam bed me23 )
    local input=""
    if [[ ! " ${has_is_data_type[*]} " =~ " $ext " ]]; then
        input="--input ${file##*.}"
    fi

    cat $file | $genozip ${args[@]:1} $input -X --force --output $output - || exit 1
    $genounzip --test $output

    # verify not generic
    if [[ $input != "" ]] && [[ $input != "--input generic" ]] && [[ `$genocat $output --show-data-type` == "GENERIC" ]]; then
        echo "data_type of $file unexpectedly not recognized and compressed as GENERIC"
        exit 1
    fi

    # note: no cleanup, we still need the output files
}

test_unix_style() {  # $1=filename  ; optional $2=rm
    test_header "$1 - basic test - Unix-style end-of-line"
    local file=$TESTDIR/$1

    if [ ! -f $file ] ; then echo "$1: File $file not found"; exit 1; fi

    cat $file | tr -d "\r" > $OUTDIR/unix-nl.$1 || exit 1
    $genozip $OUTDIR/unix-nl.$1 -ft -o $output || exit 1    

    # no cleanup as we need the output for test_windows_style
}

test_windows_style() {  # $1=filename 
    if [ -n "$is_mac" ]; then return ; fi  # note: can't run this test on mac - sed on mac doesn't recognize \r

    test_header "$1 - Window-style end-of-line"
    local file=$TESTDIR/$1

    if [ ! -f $file ] ; then echo "$1: File $file not found"; exit 1; fi

    sed 's/$/\r/g' $OUTDIR/unix-nl.$1 > $OUTDIR/windows-nl.$1 || exit 1 # note: sed on mac doesn't recognize \r
    $genozip $OUTDIR/windows-nl.$1 -ft -o $output || exit 1
    cleanup
}

test_stdout()
{
    test_header "$1 - redirecting stdout"
    local file=$TESTDIR/$1
    local ext=${file##*.}
    local arg;

    if [ "$ext" = "bam" ]; then 
        local arg='--bam -z0'
        local cmd='cat'
    else
        local cmd='tr -d \r'
    fi

    $genozip ${file} -Xfo $output || exit 1
    ($genocat --no-pg $output $arg || exit 1) | $cmd > $OUTDIR/unix-nl.$1 

    cmp_2_files $file $OUTDIR/unix-nl.$1
    cleanup
}

test_optimize()
{
    test_header "$1 --optimize"
    local file=$TESTDIR/$1
    $genozip $file -tf --optimize -o $output || exit 1
    cleanup
}

test_md5()
{
    test_header "$1 --md5 - see that it is the correct MD5"
    local file=$TESTDIR/$1

    $genozip $file -Xf --md5 -o $output || exit 1
    
    local genozip_md5 real_md5
    genozip_md5=`$genols $output | grep $output | cut -c 51-82` || exit $? 
    real_md5=`$zmd5 $file` || exit $?

    if [[ "$genozip_md5" != "$real_md5" ]]; then echo "FAILED - expected $file to have MD5=\"$real_md5\" but genozip calculated MD5=\"$genozip_md5\""; exit 1; fi

    cleanup
}

test_translate_sam_to_bam_to_sam() # $1 bam file $2 genozip options $3 genocat options
{
    test_header "$1 - translate SAM to BAM to SAM \"$2\" \"$3\""

    local bam=$TESTDIR/$1
    local sam=${bam%.bam}.sam
    local new_bam=$OUTDIR/copy_new.bam
    local new_sam=$OUTDIR/copy_new.sam
    if [ ! -f $bam ] ; then echo "$bam: File not found"; exit 1; fi
    if [ ! -f $sam ] ; then echo "$sam: File not found"; exit 1; fi

    # SAM -> BAM
    echo "STEP 1: sam -> sam.genozip"
    $genozip -fX $sam -o $output $2 || exit 1

    echo "STEP 2: sam.genozip -> bam"
    $genocat $output --bam --no-PG -fo $new_bam $3 || exit 1

    # BAM -> SAM
    echo "STEP 3: bam -> bam.genozip"
    $genozip -fX $new_bam -o $output $2 || exit 1

    echo "STEP 4: bam.genozip -> sam"
    $genocat $output --sam --no-PG -fo $new_sam $3 || exit 1

    # compare original SAM and SAM created via sam->sam.genozip->bam->bam.genozip->sam
    echo "STEP 5: compare original and output SAMs"
    cmp_2_files $sam $new_sam

    cleanup
}

verify_is_fastq() # $1 fastq file name
{
    local pluses=`egrep "^\+$" $fastq | wc -l`
    local lines=`cat $fastq | wc -l`

    if (( $pluses * 4 != $lines )); then 
        echo "After converting $1 to FASTQ: $fastq has $pluses '+' lines and $lines lines, but expecting $(($pluses * 4)) lines"
        exit 1
    fi

    # test file structure by segging
    $genozip --seg-only $fastq || exit 1
}

view_file()
{
    if [[ $1 =~ \.gz$ ]]; then  
        gunzip -c $1 || exit 1
    else
        cat $1
    fi 
}

batch_print_header()
{
    echo "***************************************************************************"
    echo "******* ${FUNCNAME[1]} (batch_id=${GENOZIP_TEST}) " $1
    echo "***************************************************************************"
}

# minimal files - expose edge cases where fields have only 1 instance
batch_minimal()
{
    batch_print_header
    local files=( minimal.vcf minimal.sam minimal.bam minimal.cram minimal.fq minimal.fa minimal.gvf minimal.me23 )
    local file
    for file in ${files[@]}; do
        test_standard " " " " $file
    done
}

# basic files
batch_basic()
{
    batch_print_header

    local save_genozip=$genozip
    if [ "$2" == latest ]; then genozip="$genozip_latest"; fi

    local file replace
    file=$1

    # if [ $file == basic.chain ]; then
    #     export GENOZIP_REFERENCE=${hs37d5}:${GRCh38}
    # else
    #     unset GENOZIP_REFERENCE
    # fi

    test_standard "" "" $file

    test_md5 $file # note: basic.bam needs to be non-BGZF for this to pass

    test_standard "--best" "" $file
    test_standard "--fast" "" $file
    test_standard "--low-memory" "" $file
    
    if [ "$file" != basic.bam ] && [ "$file" != basic.generic ]; then # binary files have no \n 
        test_unix_style $file
        test_windows_style $file
        replace=REPLACE
    else
        replace=
    fi

    test_standard "NOPREFIX CONCAT $ref" " " file://${path}${TESTDIR}/$file
    test_standard "-p123 $ref" "--password 123" $file
    if [ -z "$is_windows" ] || [ "$file" != basic.bam ]; then # can't redirect binary files in Windows
        test_redirected $file
        test_stdout $file
    fi
    test_standard "COPY $ref" " " $file

    test_optimize $file
    unset GENOZIP_REFERENCE

    genozip=$save_genozip
}

# pre-compressed files (except BGZF) and non-precompressed BAM
batch_precompressed()
{
    batch_print_header
    local files=(basic-gzip.sam.gz basic-bz2.sam.bz2 basic-xz.sam.xz) 
    local file
    for file in ${files[@]}; do

        if [ -x "$(command -v xz)" -o "${file##*.}" != xz ] ; then # skip .xz files if xz is not installed
            test_standard " " " " "$file"
            test_standard "NOPREFIX CONCAT" " " file://${path}${TESTDIR}/$file
            test_standard "-p123" "--password 123" $file
        fi
    done
}

verify_bgzf() # $1 file that we wish to inspect $2 expected result (0 not-bgzf 1 bgzf 2 BAM-bgzf-without-compression)
{
    # case: file is BGZF-foramt
    if [ "$(head -c4 $1 | od -x | head -1 | awk '{$2=$2};1')" == "0000000 8b1f 0408" ]; then 
        if [ $2 -eq 0 ]; then
            echo "$1 is unexpectedly BGZF-compressed"
            exit 1
        
        # case: BGZF format consists of non-compressed blocks
        elif [ "$(head -c26 $1 | tail -c3)" == "BAM" ]; then
            if [ $2 -eq 1 ]; then
                echo "$1 is unexpectedly a BAM file with non-compressed BGZF blocks"
                exit 1
            fi

        # case: BGZF format consists of compressed blocks
        else
            if [ $2 -eq 2 ]; then
                echo "$1 is unexpectedly a BGZF-compressed file (with compressed BGZF blocks)"
                exit 1
            fi
        fi

    else
        if [ $2 -ne 0 ]; then
            echo "$1 is not BGZF-compressed"
            exit 1
        fi
    fi
}

# bgzf files
batch_bgzf()
{
    batch_print_header

    local files=(basic-bgzf.bam basic-bgzf-6.sam.gz basic-bgzf-9.sam.gz basic-bgzf-6-no-eof.sam.gz basic-1bgzp_block.bam)
    #local files=()
    local file
    for file in ${files[@]}; do
        test_standard " " " " $file
        test_standard "NOPREFIX CONCAT" " " file://${path}${TESTDIR}/$file
        test_standard "-p123" "--password 123" $file
#        if [ -z "$is_windows" ]; then # in windows, we don't support redirecting stdin
            test_redirected $file
#        fi
        test_standard "COPY" " " $file
    done

    test_header "sam -> sam.genozip -> genocat to sam.gz - see that it is BGZF"
    local sam_gz=${OUTDIR}/bgzf_test.sam.gz
    $genozip ${TESTDIR}/basic.sam -Xfo $output || exit 1
    $genocat --no-pg $output -fo $sam_gz || exit 1
    verify_bgzf $sam_gz 1

    test_header "sam-> sam.genozip -> genocat to bam - see that it is BGZF"
    local bam=${OUTDIR}/bgzf_test.bam
    $genocat --no-pg $output -fo $bam || exit 1
    verify_bgzf $bam 1

    test_header "sam.gz -> sam.genozip -> genocat to sam - see that it is not BGZF"
    local sam=${OUTDIR}/bgzf_test.sam
    $genozip $sam_gz -Xfo $output || exit 1
    $genocat --no-pg $output -fo $sam || exit 1
    verify_bgzf $sam 0

    test_header "sam.gz -> sam.genozip -> genounzip to sam.gz - see that it is BGZF"
    local sam_gz2=${OUTDIR}/bgzf_test2.sam.gz
    $genozip $sam_gz -Xfo $output || exit 1
    $genounzip $output -fo $sam_gz2 || exit 1
    verify_bgzf $sam_gz2 1

    test_header "bam -> bam.genozip -> genounzip to bam - see that it is BGZF"
    local bam2=${OUTDIR}/bgzf_test2.bam
    $genozip $bam -Xfo $output || exit 1
    $genounzip $output -fo $bam2 || exit 1
    verify_bgzf $bam2 1

    test_header "bam -> bam.genozip -> genounzip -z0 to bam - see that it is BGZF_0"
    $genounzip $output -z0 -fo $bam2 || exit 1
    verify_bgzf $bam2 2

    # test with gencomp
    file=special.sag-by-sa.bam.gz
    $genozip ${TESTDIR}/$file -Xfo $output --force-gencomp || exit 1
    $genounzip $output -fo ${OUTDIR}/$file || exit 1
    verify_bgzf ${OUTDIR}/$file 1
}

batch_subdirs()
{
    batch_print_header
    local files=(${TESTDIR}/minimal.sam ${TESTDIR}/basic-subdirs)

    cleanup
    $genozip -Dft ${files[*]} || exit 1

    # verify that the file residing in the subdir was compressed
    $genounzip -f ${TESTDIR}/basic-subdirs/basic.subdirs.txt.genozip || exit
    cleanup
}   
        
# files represent cases that cannot be written into the test files because they would conflict
batch_special_algs()
{
    batch_print_header
    local files=(basic-domqual.fq basic-domqual.sam basic-unaligned.sam basic-no-samples.vcf)
    local file
    for file in ${files[@]}; do
        test_unix_style $file                # standard
        test_standard "-p123" "-p 123" $file # encrypted
        test_standard "COPY" " " $file       # multiple files unbound
        test_optimize $file                  # optimize 
    done

    test_header "FASTQ QUAL with + regression test"
    $genozip ${TESTDIR}/regression.has-+-qual.fq -B16 -fX || exit 1 # regression test for bug of parsing FASTQ that has QUAL lines that start with a +

    test_header "LONGR edge case regression test"
    $genozip ${TESTDIR}/regression.longr-issue.bam -ft || exit 1 

    test_header "VCF empty sample fields regression test"
    $genozip ${TESTDIR}/regression.empty-sample-fields.vcf -ft || exit 1 

    # bug was: AF subfield in CSQ was segged to INFO/AF, messing up COPY_OTHER(INFO_AF) in FORMAT_AF - in v15 until 15.0.60
    test_header "CSQ has an AF field"
    $genozip ${TESTDIR}/regression.CSQ_has_AF.vcf -ft || exit 1 

    # bug was: piz failed if two consecutive Is in CIGAR (ignoring non-seq-consuming ops like N) (defect 2024-06-16)
    test_header "two consecutive Is in CIGAR" 
    $genozip ${TESTDIR}/regression.two-consecutive-Is.sam -ft || exit 1 

    # bug was: MC copying CIGAR from mate, when both are "*" (=empty in BAM). Fixed in 15.0.62 in PIZ.
    test_header "MC copying CIGAR from mate, when both are null-CIGARs"
    $genozip -tf ${TESTDIR}/regression.2024-06-26.MC-copy-from-mate-CIGAR.null-CIGAR.bam || exit 1

    # txt data is 393k - over segconf's first VB size of 300K (test in plain, bgzf, gz; test SAM and FASTQ as they have different code paths related to discovery of gz codec)
    test_header "Single read/alignment longer than segconf's first VB"
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.plain.sam   || exit 1
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.bgzf.sam.gz || exit 1
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.gz.sam.gz   || exit 1
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.plain.fq    || exit 1
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.bgzf.fq.gz  || exit 1
    $genozip -tf ${TESTDIR}/special.force-2nd-segconf-vb_size.gz.fq.gz    || exit 1
}

batch_qual_codecs()
{
    batch_print_header

    local codecs=( normq domq longr pacb smux homp )
    local files=( special.depn.sam test.cigar-no-seq-qual.bam )

    for file in ${files[@]}; do
        for codec in ${codecs[@]}; do
            test_header "Testing QUAL codec ${codec^^} in $file WITHOUT gencomp"
            $genozip $TESTDIR/$file -ft --no-gencomp --force-$codec || exit 1

            test_header "Testing QUAL codec ${codec^^} in $file WITH gencomp"
            $genozip $TESTDIR/$file -ft --force-gencomp --force-$codec || exit 1
        done
    done
}

# unit test for ref_copy_compressed_sections_from_reference_file. 
batch_copy_ref_section()
{
    batch_print_header

    #created with -r1:9660000-10650000, and contains 99% of vb=11 of hs37d5.ref.genozip which is 3867649-4834572
    local file=${TESTDIR}/unit-test.-E.copy-ref-section.sam.gz

    $genozip -E $hs37d5 -p 123 -ft $file -fo $output || exit 1

    cleanup
}    

# test -@1 - different code paths
batch_single_thread()
{
    batch_print_header

    # note -@1 will override previous -@
    test_standard "-@1" "-@1" basic.vcf 
    
    cleanup
}

batch_iupac() 
{
    batch_print_header

    # SAM - genocat and verifying with wc
    non_iupac_lines=$(( `grep -v "^@" ${TESTDIR}/basic.sam | wc -l` - 1 )) # we have 1 IUPAC line (E100020409L1C001R0030000234)

    test_header "genocat --bases ACGTN (SAM)"
    test_count_genocat_lines ${TESTDIR}/basic.sam "-H --bases=ACGTN" $non_iupac_lines

    test_header "genocat --bases ^ACGTN (SAM)"
    test_count_genocat_lines ${TESTDIR}/basic.sam "-H --bases=^ACGTN" 1

    # SAM - using --count
    test_header "genocat --bases ACGTN --count (SAM)"
    local count # seperate from assignment to preserve exit code
    count=`$genocat_no_echo $output -H --bases ACGTN --count -q` || exit $? 
    if [ "$count" == "" ]; then echo genocat error; exit 1; fi

    if [ "$count" -ne $non_iupac_lines ]; then echo "bad count = $count, expecting $non_iupac_lines"; exit 1; fi

    test_header "genocat --bases ^ACGTN --count (SAM)"
    count=`$genocat_no_echo $output -H --bases ^ACGTN --count -q` || exit $? 
    if [ "$count" -ne 1 ]; then echo "bad count = $count"; exit 1; fi

    # BAM - using --count
    test_header "genocat --bases ACGTN --count --bam"
    count=`$genocat_no_echo $output --bam --bases ACGTN --count -q` || exit $? 
    if [ "$count" == "" ]; then echo genocat error; exit 1; fi

    if [ "$count" -ne $non_iupac_lines ]; then echo "bad count = $count, expecting $non_iupac_lines"; exit 1; fi

    test_header "genocat --bases ^ACGTN --count --bam"
    count=`$genocat_no_echo $output --bam --bases ^ACGTN --count -q` || exit $? 
    if [ "$count" -ne 1 ]; then echo "bad count = $count"; exit 1; fi

    # FASTQ (verifying with wc)
    test_count_genocat_lines ${TESTDIR}/basic.fq "-H --IUPAC=ACGTN" 20
    test_count_genocat_lines ${TESTDIR}/basic.fq "-H --IUPAC=^ACGTN" 4
    non_iupac_lines=5
    
    # FASTQ - using --count
    test_header "genocat --bases ACGTN --count (FASTQ)"
    count=`$genocat_no_echo $output -H --bases ACGTN --count -q` || exit $? 
    if [ "$count" == "" ]; then echo genocat error; exit 1; fi

    if [ "$count" -ne $non_iupac_lines ]; then echo "bad count = $count, expecting $non_iupac_lines"; exit 1; fi

    test_header "genocat --bases ^ACGTN --count (FASTQ)"
    count=`$genocat_no_echo $output -H --bases ^ACGTN --count -q` || exit $? 
    if [ "$count" -ne 1 ]; then echo "bad count = $count"; exit 1; fi
}

# Test SAM/BAM translations
batch_sam_bam_translations()
{
    batch_print_header

    # test different buddy code path for subsetted file
    test_translate_sam_to_bam_to_sam special.buddy.bam " " -r22

    # test with gencomp
    test_translate_sam_to_bam_to_sam special.depn.bam --force-gencomp

    # note: we have these files in both sam and bam versions generated with samtools
    local files=(special.buddy.bam 
                 special.depn.bam            # depn/prim with/without QUAL
                 special.NA12878.bam 
                 special.pacbio.ccs.bam      # unaligned SAM/BAM with no SQ records
                 special.human2.bam          
                 special.bsseeker2-rrbs.bam) # sam_piz_special_BSSEEKER2_XM sensitive to SAM/BAM
    local file
    for file in ${files[@]}; do
        test_translate_sam_to_bam_to_sam $file
    done
}

# Test --coverage, --idxstats- not testing correctness, only that it doesn't crash
batch_coverage_idxstats()
{
    batch_print_header

    # note: we have these files in both sam and bam versions generated with samtools
    local files=(special.buddy.bam 
                 special.depn.bam        # depn/prim with/without QUAL
                 special.NA12878.bam 
                 special.pacbio.ccs.bam  # unaligned SAM/BAM with no SQ records
                 special.human2.bam
                 special.collated.bam)
    local file
    for file in ${files[@]}; do
        $genozip -Xf $TESTDIR/$file -o $output               || exit 1
        $genocat $output --idxstats > $OUTDIR/$file.idxstats || exit 1
        $genocat $output --coverage > $OUTDIR/$file.coverage || exit 1
    done

    cleanup
}

batch_qname_flavors()
{
    batch_print_header

    local files=( `cd $TESTDIR/flavors; ls -1 flavor.* | \
                   grep -vF .genozip | grep -vF .md5 | grep -vF .bad ` ) 

    local file
    for file in ${files[@]}; do
        test_header "Testing QNAME flavor of $file"
        
        local expected_flavor=`echo $file | cut -d. -f2`
        
        $genozip "$TESTDIR/flavors/$file" -Wft -o $output --debug-qname > $OUTDIR/stats || exit 1
        local observed_flavor=`grep "Read name style:" $OUTDIR/stats | cut -d" " -f4`

        if [[ "$expected_flavor" != "$observed_flavor" ]]; then
            cat $OUTDIR/stats
            echo "$file: Incorrect flavor. Filename indicates \"$expected_flavor\" but genozip found \"$observed_flavor\""
            exit 1
        fi
    done

    cleanup
}

batch_piz_no_license()
{
    batch_print_header

    $genozip $TESTDIR/minimal.vcf -fX || exit 1

    rm -f $LICFILE
    $genounzip -t $TESTDIR/minimal.vcf.genozip || exit 1

    cleanup
}

batch_sendto()
{
    batch_print_header

    local lic_num=`tail -1 $LICENSESDIR/genozip_license.v15.Premium | sed "s/[^0-9]//g"`

    test_header "sender compresses+tests, receiver uncompresses\n"
    install_license SendTo || exit 1
    $genozip $TESTDIR/minimal.vcf -ft --sendto $lic_num || exit 1

    install_license Premium || exit 1
    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output || exit 1

    test_header "sender compresses+tests with encryption, receiver uncompresses\n"
    install_license SendTo || exit 1
    $genozip $TESTDIR/minimal.vcf -ft -p xyz --sendto $lic_num || exit 1

    install_license Premium || exit 1
    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output -p xyz || exit 1

    test_header "wrong license number - expecting ACCESS DENIED\n"
    install_license SendTo || exit 1
    $genozip $TESTDIR/minimal.vcf -ft --sendto 1234 || exit 1

    install_license Premium || exit 1
    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output 
    verify_failure genounzip $?

    test_header "no license - expecting ACCESS DENIED\n"
    install_license SendTo || exit 1
    $genozip $TESTDIR/minimal.vcf -ft --sendto 1234 || exit 1
    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output
    verify_failure genounzip $?

    test_header "Academic license - expecting ACCESS DENIED\n"
    install_license SendTo || exit 1
    local lic_num=`tail -1 $LICENSESDIR/genozip_license.v15.Academic | sed "s/[^0-9]//g"`

    $genozip $TESTDIR/minimal.vcf -ft --sendto $lic_num || exit 1
    install_license Academic || exit 1

    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output
    verify_failure genounzip $?

    test_header "Enteprise license w/ no-eval - expecting ACCESS DENIED\n"
    install_license SendTo || exit 1
    local lic_num=`tail -1 $LICENSESDIR/genozip_license.v15.Enterprise | sed "s/[^0-9]//g"`

    $genozip $TESTDIR/minimal.vcf -ft --sendto $lic_num || exit 1
    install_license Enterprise || exit 1

    $genounzip $TESTDIR/minimal.vcf.genozip -fo $output --no-eval
    verify_failure genounzip $?

    cleanup
}

batch_user_message_permissions()
{
    batch_print_header

    local msg=$TESTDIR/user-message
    local line="`head -1 $msg`"
    local file=$TESTDIR/minimal.vcf
    local recon=$OUTDIR/recon.vcf
    local recon_msg=$OUTDIR/msg

    test_header "test: successful message: generated with Premium, read without a license"
    install_license Premium
    $genozip $file --user-message $msg -fXo $output || exit 1

    rm -f $LICFILE

    $genounzip $output -fo $recon > $recon_msg || exit 1
    if (( `grep "$line" $recon_msg | wc -l` != 1 )); then
        echo "Error: cannot find message from $msg in $recon_msg"
        exit 1
    fi

    test_header "test: failed message: generated with Enterprise"
    install_license Enterprise
    $genozip $file --user-message $msg -fXo $output --no-eval
    verify_failure genozip $?

    cleanup
}

batch_password_permissions()
{
    batch_print_header

    local file=$TESTDIR/minimal.vcf
    local recon=$OUTDIR/recon.vcf
    local recon_msg=$OUTDIR/msg

    test_header "test: successful encryption: generated with Premium, read with Academic"
    install_license Premium
    $genozip $file --password 1234567890qwertyuiop -fXo $output || exit 1

    install_license Academic
    $genounzip $output --password 1234567890qwertyuiop -fo $recon > $recon_msg || exit 1

    test_header "test: failed decryption: wrong password"
    $genounzip $output --password wrong_password -fo $recon > $recon_msg
    verify_failure genounzip $?

    test_header "test: failed encryption in Academic"
    $genozip $file --password 1234567890qwertyuiop -fXo $output 
    verify_failure genozip $?

    cleanup
}

# Test 23andMe translations
# note: only runs it to see that it doesn't crash, doesn't validate results
batch_23andMe_translations()
{
    batch_print_header

    local file=test.genome_Full.txt
 
    test_header "$file - translate 23andMe to VCF"

    local me23=$TESTDIR/$file
    local vcf=$OUTDIR/copy.vcf.gz

    $genozip -Xf $me23 -o $output         || exit 1
    $genocat $output -fo $vcf -e $hs37d5 || exit 1

    cleanup
}

batch_genocat_tests()
{
    batch_print_header

    # FASTA genocat tests
    local file=$TESTDIR/basic.fa
    test_count_genocat_lines $file "--sequential" 9
    test_count_genocat_lines $file "--header-only" 3
    test_count_genocat_lines $file "--header-one" 18
    test_count_genocat_lines $file "--no-header" 15
    test_count_genocat_lines $file "--no-header --sequential" 6
    test_count_genocat_lines $file "--grep cytochrome" 6
    test_count_genocat_lines $file "--grep cytochrome --sequential " 2
    test_count_genocat_lines $file "--grep cytochrome --sequential --no-header " 1

    # FASTQ genocat tests
    local file=$TESTDIR/basic.fq
    local num_lines=`grep + $file | wc -l`
    test_count_genocat_lines $file "--header-only" $num_lines 
    test_count_genocat_lines $file "--seq-only" $num_lines 
    test_count_genocat_lines $file "--qual-only" $num_lines 
    test_count_genocat_lines $file "--downsample 2" $(( 4 * $num_lines / 2 )) 
    test_count_genocat_lines "--pair -E $GRCh38 $file $file" "--interleave" $(( 4 * $num_lines * 2 )) 
    test_count_genocat_lines "--pair -E $GRCh38 $file $file" "--interleave --downsample=5,4" $(( 4 * $num_lines / 5 * 2 )) 
    test_count_genocat_lines "--pair -E $GRCh38 $file $file" "--grep PRFX --header-only" 2
    test_count_genocat_lines "--pair -E $GRCh38 $file $file" "--R1" $(( 4 * $num_lines )) 
    test_count_genocat_lines "--pair -E $GRCh38 $file $file" "--R2" $(( 4 * $num_lines ))

    # test --interleave and with --grep
    sed "s/PRFX/prfx/g" $file > $OUTDIR/prfx.fq
    test_count_genocat_lines "--pair -E $GRCh38 $file $OUTDIR/prfx.fq" "--interleave=either --grep PRFX" 8
    test_count_genocat_lines "--pair -E $GRCh38 $file $OUTDIR/prfx.fq" "--interleave=both --grep PRFX" 0

    # grep without pairing
    test_count_genocat_lines $file "--grep line5 --header-only" 1

    # BED genocat tests
    local file=$TESTDIR/basic.bed
    test_count_genocat_lines $file "--header-only" 3
    test_count_genocat_lines $file "--no-header" 7
    test_count_genocat_lines $file "--grep UBXN11 --no-header" 2
    test_count_genocat_lines $file "--lines=2-4 --no-header" 3
    test_count_genocat_lines $file "--head=2 --no-header" 2
    test_count_genocat_lines $file "--tail=2 --no-header" 2

    # SAM genocat tests
    local file=$TESTDIR/basic.sam
    local filter=$TESTDIR/basic.sam.qname-filter
    test_count_genocat_lines $file "-H --qnames-file $filter" 4
    test_count_genocat_lines $file "-H --qnames-file ^$filter" 11

    local filter_opt=`cut -d" " -f1 $filter | sed "s/^@//g" | tr "\n" ","`
    test_count_genocat_lines $file "-H --qnames $filter_opt" 4
    test_count_genocat_lines $file "-H --qnames ^$filter_opt" 11

    local filter=$TESTDIR/basic.sam.seq-filter
    test_count_genocat_lines $file "-H --seqs-file $filter" 4
    test_count_genocat_lines $file "-H --seqs-file ^$filter" 11
    
    # BAM genocat tests
    local file=$TESTDIR/basic.bam
    local filter=$TESTDIR/basic.sam.qname-filter
    test_count_genocat_lines $file "--sam -z0 -H --qnames-file $filter" 4
    test_count_genocat_lines $file "--sam -z0 -H --qnames-file ^$filter" 11

    local filter=$TESTDIR/basic.sam.seq-filter
    test_count_genocat_lines $file "--sam -z0 -H --seqs-file $filter" 4
    test_count_genocat_lines $file "--sam -z0 -H --seqs-file ^$filter" 11
}

# test --grep, --count, --lines
batch_grep_count_lines()
{
    batch_print_header

    local file
    for file in ${basics[@]}; do
        if [ $file == basic.fa ] || [ $file == basic.bam ] || [ $file == basic.locs ] ||\
           [ $file == basic.generic ] || [ $file == basic.cram ] || [ $file == basic.bcf ]; then continue; fi

        # number of expected lines
        local lines=1
        if [ $file == basic.fq ]; then lines=4; fi

        # grep        
        test_count_genocat_lines $TESTDIR/$file "--grep PRFX --no-header" $lines
        test_count_genocat_lines $TESTDIR/$file "--grep NONEXISTANT --no-header" 0

        # count
        $genozip $TESTDIR/$file -Xfo $output || exit 1
    
        local count # seperate from assignment to preserve exit code
        count=`$genocat_no_echo --quiet --count $output` || exit $? 
        if [ "$count" == "" ]; then echo genocat error; exit 1; fi

        # lines
        test_count_genocat_lines $TESTDIR/$file "--no-header --lines=${count}-${count}" $lines
        test_count_genocat_lines $TESTDIR/$file "--no-header --lines=100000-" 0

        unset GENOZIP_REFERENCE
    done

    # regions-file
    test_count_genocat_lines "$TESTDIR/basic.vcf" "-R $TESTDIR/basic.vcf.regions -H" 7

    # regions
    test_count_genocat_lines "$TESTDIR/basic.vcf" "--regions 13:207237509-207237510,1:207237250 -H" 7
}

ass_eq_num() # $1 result $2 expected
{
    local res=`echo "$1" | tr -d " "` # on mac, wc -l includes leading spaces

    echo \"$res\" \"$2\"
    if ! [[ "$res" =~ ^[0-9]+$ ]] ; then
        echo "test.sh: Failed: result \"$res\" is not a number" # genocat failed and hence didn't return a number - error message is already displayed
        exit 1
    fi

    if ! [[ "$res" =~ ^[0-9]+$ ]] ; then
        echo "test.sh: Bad comparison argument, expecting \$2 to be an integer" # genocat failed and hence didn't return a number - error message is already displayed
        exit 1
    fi

    if (( $res != $2 )); then 
        echo "test.sh: Failed: result is $1 but expecting $2"
        exit 1
    fi
}

batch_bam_subsetting()
{
    batch_print_header

    # note: we use a sorted file with SA:Z to activate the gencomp codepaths
    local file=$TESTDIR/test.human2.bam.genozip
    $genozip $TESTDIR/test.human2.bam -fXB4 --force-gencomp || exit 1

    # (almost) all SAM/BAM subseting options according to: https://www.genozip.com/compressing-bam
    ass_eq_num "`$genocat $file --header-only | wc -l`" 93
    ass_eq_num "`$genocat $file -r 1 --no-header | wc -l`" 99909 # test --regions in presence of gencomp, but entire file is one contig
    ass_eq_num "`$genocat $file -r 1 --count`" 99909
    ass_eq_num "`$genocat $file --grep AS:i:150 --count`" 12297
    ass_eq_num "`$genocat $file --grep-w 1000 --count`" 24
    ass_eq_num "`$genocat $file --bases N --count`" 2              
    ass_eq_num "`$genocat $file --bases ^ACGT --count`" 76
    ass_eq_num "`$genocat $file --downsample 2 --no-header | wc -l`" 49955  # note: --downsample, --head, --tail, --lines are incomptaible with --count
    ass_eq_num "`$genocat $file --downsample 2,1 --no-header | wc -l`" 49954
    ass_eq_num "`$genocat $file --lines 5000-19999 --no-header | wc -l`" 15000 # spans more than one VB
    ass_eq_num "`$genocat $file --head=15000 --no-header | wc -l`" 15000       # spans more than one VB
    ass_eq_num "`$genocat $file --tail=15000 --no-header | wc -l`" 15000       # spans more than one VB
    ass_eq_num "`$genocat $file --FLAG=+SUPPLEMENTARY --count`" 58 # should be the same as samtools' "-f SUPPLEMENTARY"
    ass_eq_num "`$genocat $file --FLAG=^48 --count`" 99816         # should be the same as samtools' "-G 48"
    ass_eq_num "`$genocat $file --FLAG=-0x0030 --count`" 315       # should be the same as samtools' "-F 0x0030"
    ass_eq_num "`$genocat $file --MAPQ 20 --count`" 8753
    ass_eq_num "`$genocat $file --MAPQ ^20 --count`" 91156

    local file=$TESTDIR/test.human3-collated.bam.genozip
    $genozip $TESTDIR/test.human3-collated.bam -fXB4 --force-gencomp || exit 1

    if [ ! -f $file ]; then $genozip $TESTDIR/test.human3-collated.bam -fXB4 || exit 1; fi
    ass_eq_num "`$genocat $file -r chr1 --no-header | wc -l`" 4709 # test --regions - real subsetting, but no gencomp as it is collated
    ass_eq_num "`$genocat $file -r chr1 --count`" 4709

    # TO DO: combinations of subsetting flags
}

batch_backward_compatability()
{
    batch_print_header
    local files=( `ls -r $TESTDIR/back-compat/[0-9]*/*.genozip | grep -v "/0" | grep -v .ref.genozip` ) # since v15, backcomp goes back only to v11 
    local file ref

    # standard reference are taken from REFDIR, regression-test references which are
    # not found in REFDIR, are taken specified in the genozip file
    export GENOZIP_REFERENCE=$REFDIR

    for file in ${files[@]}; do
        test_header "$file - backward compatability test"

        $genounzip --no-cache -t $file || exit 1
    done

    cleanup # to do: change loop ^ to double loop, clean up after each version (to remove shm)
}
    
batch_real_world_1_adler32() # $1 extra genozip argument
{
    batch_print_header

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    local filter_xz=nothing
    if [ ! -x "$(command -v xz)" ] ; then # xz unavailable
        local filter_xz=.xz
    fi

    local filter_zip=nothing
    if [ ! -x "$(command -v unzip)" ] ; then # xz unavailable
        local filter_zip=.zip
    fi

    local filter_bcf=nothing
    if [ ! -x "$(command -v bcftools)" ] || [ -n "$is_exe" ]; then # bcftools unavailable
        local filter_bcf=.bcf
    fi

    local debug_filter=nothing
    if [ -n "$is_debug" ]; then 
        local debug_filter="test.1M-@SQ.sam" # -fsanitize=address hangs on this file
    fi

    # without reference
    local files=( `cd $TESTDIR; ls -1 test.*vcf* test.*bcf* test.*sam* test.*bam test.*fq* test.*fa*                \
                   test.*gvf* test.*gtf* test.*gff* test.*locs* test.*bed* test.*txt* test.*.pbi                    \
                   | grep -v "$filter_xz" | grep -v "$filter_zip" | grep -v "$filter_bcf" | grep -v "$debug_filter" \
                   | grep -vE "headerless|\.genozip|\.md5|\.bad|\.ora" ` )

    # test genozip and genounzip --test
    echo "subsets of real world files (without reference)"
    test_standard "-f $1 --show-filename" " " ${files[*]}

    # don't remove .genozip files as we will use them in batch_real_world_genounzip_*
    #for f in ${files[@]}; do rm -f ${f}.genozip; done
}

batch_real_world_optimize() 
{
    batch_print_header

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    cd $TESTDIR
    local files=( `ls -1 test.*vcf test.*vcf.gz test.*bcf test.*sam test.*sam.gz test.*bam test.*fq test.*fq.gz |
                   grep -v headerless` )

    # test genozip and genounzip --test - first 10K lines of the file should be sufficient to detect optimization issues
    echo "compressing first 10k lines with --optimize"
    $genozip --head=10000 --optimize --show-filename --test --force ${files[*]} || exit 1

    cd -
}

test_effective_codec_pair() # $1=R1_file $2=R2_file $3=expected_effective_codecs 
{
    test_header "Test effective codes of paired $3 files"
    local effective_codecs=$OUTDIR/effective_codecs.txt

    $genozip "$TESTDIR/$1" "$TESTDIR/$2" -2fX -o $output -e $hs37d5 --show-bgzf | grep effective_codec | cut -d= -f3 | cut -d" " -f1 | tr "\n" " " > $effective_codecs || exit 1 # not in sub-shell so we can catch a genozip error 
    (( ${PIPESTATUS[0]} == 0 )) || exit 1

    echo -n "effective_codecs=" `cat $effective_codecs`

    if [[ "`cat $effective_codecs`" != "$3" ]]; then
        echo "expected_effective_codecs=\"$3\" but effective_codecs=\"`cat $effective_codecs`\""
        exit 1
    fi

    # test genounzip too for good measure
    $genounzip -t $output || exit 1
}

# MGZIP codecs 
batch_mgzip_fastq() 
{
    batch_print_header

    local files=( gz.bgzf.truncated.fq.gz gz.il1m.illumina.truncated.fq.gz gz.mgzf.mgi.R1.fq.gz \
                  gz.mgsp.mgi.R1.fq.gz gz.emfl.element.fq.gz gz.emvl.element.R1.fq.gz )
    local recon=$OUTDIR/recon.fq
    local truncated=$OUTDIR/truncated.fq
    local discovered_codec=$OUTDIR/discovered_codec.txt

    for f in ${files[@]}; do 
        test_header "Test codec discovery $f"
        local expected_codec=`echo $f | cut -d. -f2`

       $genozip --show-gz $TESTDIR/$f -X | grep src_codec= |cut -d= -f2 | cut -d" " -f1 > $discovered_codec # not subshell so we can catch a genozip error
       (( ${PIPESTATUS[0]} == 0 )) || exit 1

        if (( "${expected_codec^^}" != "`cat $discovered_codec`")); then
            echo "$f: Bad codec discovery for file: expected_codec=\"${expected_codec^^}\" discovered_codec=\"`cat $discovered_codec`\""
            exit 1
        fi
        echo "$f is compressed with effective_codec=`cat $discovered_codec`"

        test_header "$f: Test reconstruction"
        if (( `echo $f | grep truncated | wc -l` == 0 )); then
            $genozip -fX $TESTDIR/$f || exit 1
            $genounzip $TESTDIR/${f/.gz/.genozip} -fo $recon || exit 1
            cmp_2_files $TESTDIR/$f $recon
        else
            $genozip -fX --truncate $TESTDIR/$f || exit 1 # full gz blocks, but last block has a partial read
            $genounzip $TESTDIR/${f/.gz/.genozip} -fo $recon || exit 1
            $zcat $TESTDIR/$f | head -$(( `$zcat $TESTDIR/$f | wc -l` / 4 * 4 )) > $truncated
            cmp_2_files $truncated $recon
        fi


        test_header "$f: --truncated: last mgzip block is truncated"

        local len=`ls -l $TESTDIR/$f | cut -d" " -f5`
        trunc_f=$OUTDIR/truncated.$f
        head -c $(( len - 10 )) $TESTDIR/$f > $trunc_f 

        $genozip -ft --truncate $trunc_f || exit 1
    done

    test_header "drastically different VB sizes in pair: short VBs first"
    $genozip -ft $TESTDIR/special.human2-R2.short-seqs.fq.gz $TESTDIR/test.human2-R1.fq.gz -2 -e $hs37d5

    test_header "drastically different VB sizes in pair: long VBs first"
    $genozip -ft $TESTDIR/test.human2-R1.fq.gz $TESTDIR/special.human2-R2.short-seqs.fq.gz -2 -e $hs37d5

    # two special code paths for handling truncated GZIL files, depending if the garbage last word of the file >1MB (detected during read) or <=1MB (detected during uncompress) 
    test_header "truncated IL1M file: fake isize in last word > 1MB"
    $genozip -tf --truncate ${TESTDIR}/special.il1m.truncated-last-word.gt.1MB.fastq.gz || exit 1 

    test_header "truncated IL1M file: fake isize in last word <= 1MB"
    $genozip -tf --truncate ${TESTDIR}/special.il1m.truncated-last-word.eq.1MB.fastq.gz || exit 1 

    local files=( gz.bgzf.truncated.fq.gz gz.il1m.illumina.truncated.fq.gz )
    for f in ${files[@]}; do 
        test_header "$f: --truncated: Full mgzip blocks, but last read of last block is truncated"
        $genozip -ft --truncate $TESTDIR/$f || exit 1
    done

    # check that pair effective codecs are as intended
    test_effective_codec_pair il1m.human2-R1.fq.gz il1m.human2-R2.fq.gz "IL1M IL1M "
    test_effective_codec_pair test.human2-R1.fq.gz test.human2-R2.fq.gz "BGZF BGZF "
    test_effective_codec_pair test.human2-R1.fq.gz il1m.human2-R2.fq.gz "BGZF GZ "
    test_effective_codec_pair gz.mgsp.mgi.R1.fq.gz gz.mgsp.mgi.R2.fq.gz "MGSP MGSP "
    test_effective_codec_pair gz.mgzf.mgi.R1.fq.gz gz.mgzf.mgi.R2.fq.gz "MGZF MGZF "
    test_effective_codec_pair gz.emvl.element.R1.fq.gz gz.emvl.element.R2.fq.gz "EMVL EMVL " 
    test_effective_codec_pair gz.mgsp.mgi.R1.fq.gz gz.bgzf.mgi.R2.fq.gz "MGSP GZ "
    test_effective_codec_pair gz.bgzf.mgi.R2.fq.gz gz.mgsp.mgi.R1.fq.gz "BGZF GZ "

    # mix an IN_SYNC codec (MGSP) with a non-in-sync (BGZF)
    $zcat $(TESTDIR)

    # regression tests

    # bug was: compression failed, bc failed to identify a read in the VB when there is only one, with part of it in unconsumed_txt from previous VB, and part in a BGZF block (fixed 15.0.56)
    test_header "regression: vb=2 is single read" 
    $genozip -B32 ${TESTDIR}/regression.vb2-is-single-read.fq.gz -ft || exit 1 

    # bug was: compression failed, bc igzip would not decompress a isize > segconf.vb_sizes[0], and segconf gave up instead of attempting vb_sizes[1]
    test_header "regression: segconf vb_sizes[0] too small, try with vb_size[1]" 
    $genozip ${TESTDIR}/regression.no_small_segconf_vb_size.fq.gz -ft --no-bgzf --truncate || exit 1 

    # bug was: igzip not handling of IL1M blocks after move to igzip (defect 2024-03-01)
    test_header "regression: ILIM with igzip: multi-gzip-break-between-reads"
    $genozip -tf --no-bgzf ${TESTDIR}/regression.defect-2024-03-01.multi-gzip-break-between-reads.fq.gz || exit 1 

    test_header "regression: ILIM with igzip: multi-gzip-break-within-read"
    $genozip -tf --no-bgzf ${TESTDIR}/regression.defect-2024-03-01.multi-gzip-break-within-read.fq.gz || exit 1 

    # bug was: CHECKSUM at the end of 1MB-gz block in R2 was not handled correctly, failing R2 zip.
    # (R1 is BGZF file containing the same number of reads as the truncated R2 IL1M file)
    test_header "regression: ILIM with igzip: R2 alignment to R1 due to CHECKSUM" 
    $genozip -tf --pair -e $hs37d5 --truncate -B19 --no-bgzf ${TESTDIR}/regression.defect-2024-06-21.R1.il1m-broke-w-B19.fq.gz \
                                                             ${TESTDIR}/regression.defect-2024-06-21.R2.il1m-broke-w-B19.fq.gz || exit 1 

    $genozip -tf --pair -e $hs37d5 --truncate -B100000B      ${TESTDIR}/regression.defect-2024-06-21.R1.il1m-broke-w-B19.fq.gz \
                                                             ${TESTDIR}/regression.defect-2024-06-21.R2.il1m-broke-w-B19.fq.gz || exit 1 

    # bug was: non-terminal BGZF EOF blocks were not handled correctly
    test_header "regression: Non-terminal BGZF EOF block"
    local file=${TESTDIR}/regression.defect-2024-07-03.midfile-bgzf-eof.vcf.gz
    $genozip -tf $file -o $output || exit 1
    $genounzip $output -fo ${OUTDIR}/recon.vcf || exit 1
    cmp_2_files $file ${OUTDIR}/recon.vcf
}


# test genounzip with many files of different types in a single process
batch_real_world_genounzip_single_process() # $1 extra genozip argument
{
    batch_print_header

    local files=( `cd $TESTDIR; ls -1 test.*.vcf.genozip test.*.sam.genozip test.*.bam.genozip \
                   test.*.fq.genozip test.*.fa.genozip \
                   test.*.gvf.genozip test.*.gtf.genozip test.*gff*.genozip \
                   test.*.locs.genozip test.*.bed.genozip \
                   test.*txt.genozip test.*.pbi.genozip |
                   grep -vF .d.vcf`)

    $genounzip ${files[@]/#/$TESTDIR/} --test || exit 1
}

batch_real_world_genounzip_compare_file() # $1 extra genozip argument
{
    batch_print_header

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    local filter_xz=nothing
    if [ ! -x "$(command -v xz)" ] ; then # xz unavailable
        local filter_xz=.xz
    fi

    local filter_zip=nothing
    if [ ! -x "$(command -v unzip)" ] ; then # xz unavailable
        local filter_zip=.zip
    fi

    local debug_filter=nothing
    if [ -n "$is_debug" ]; then 
        local debug_filter="test.1M-@SQ.sam" # -fsanitize=address hangs on this file
    fi

    # without reference
    local files=( `cd $TESTDIR; ls -1 test.*vcf* test.*sam* test.*bam \
                   test.*fq* test.*fa* \
                   test.*gvf* test.*gtf* test.*gff* test.*locs* test.*bed* \
                   test.*txt* test.*pbi \
                   | grep -v "$filter_xz" | grep -v "$filter_zip" | grep -v "$debug_filter" \
                   | grep -vE "headerless|\.genozip|\.md5|\.bad|\.ora"` )
    
    # test full genounzip (not --test), including generation of BZGF
    for f in ${files[@]}; do 

        if   [[ $f == *.gz ]];  then
            local genozip_file=${TESTDIR}/${f%.gz}.genozip
        elif [[ $f == *.zip ]]; then
            local genozip_file=${TESTDIR}/${f%.zip}.genozip
        elif [[ $f == *.xz ]];  then
            local genozip_file=${TESTDIR}/${f%.xz}.genozip
        elif [[ $f == *.bz2 ]];  then
            local genozip_file=${TESTDIR}/${f%.bz2}.genozip
        else
            local genozip_file=${TESTDIR}/$f.genozip
        fi

        # note: normally, the test runs on files compressed in batch_real_world_1_adler32 - we compress them here if not
        if [ ! -f $genozip_file ]; then
            $genozip ${TESTDIR}/$f -fX || exit 1
        fi

        local recon=${OUTDIR}/$f
        $genounzip $genozip_file -o $recon || exit 1

        local actual_md5=`$zmd5 $recon`

        local expected_md5=`cat ${TESTDIR}/${f}.md5` # calculated in test/Makefile
        if [[ "$actual_md5" != "$expected_md5" ]] ; then
            echo "${TESTDIR}/$f has MD5=$expected_md5 but reconstructed file ${OUTDIR}/$f has MD5=$actual_md5"
            exit 1
        fi

        rm -f ${OUTDIR}/$f
    done

    cleanup 
}

batch_real_world_with_ref_md5() # $1 extra genozip argument
{
    batch_print_header $1

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    local files37=( test.IonXpress.sam.gz \
                    test.human.fq.gz test.human2.bam test.human2.interleaved.fq.gz test.pacbio.clr.bam \
                    test.human2-R1.fq.bz2 test.pacbio.ccs.10k.bam test.unmapped.sam.gz \
                    test.NA12878.chr22.1x.bam test.NA12878-R1.100k.fq \
                    test.human2.filtered.snp.vcf test.solexa-headerless.sam test.cigar-no-seq-qual.bam \
                    test.platypus.vcf )

    local files19=( test.pacbio-blasr.bam )
    local files38=( test.1KG-38.vcf.gz test.human-collated-headerless.sam test.human-sorted-headerless.sam \
                    test.ultima-giab.vcf.gz )

    local filesT2T1_1=( test.nanopore.t2t_v1_1.bam )

    test_standard "-mf $1 -e $hs37d5 --show-filename" " " ${files37[*]}
    test_standard "-mf $1 -E $GRCh38 --show-filename" " " ${files38[*]}

    cleanup_cache
    test_standard "-mf $1 -e $hg19   --show-filename" " " ${files19[*]}

    cleanup_cache
    test_standard "-mf $1 -E $T2T1_1 --show-filename" " " ${filesT2T1_1[*]}
    
    for f in ${files37[@]} ${files19[@]} ${files38[@]} ${filesT2T1_1[@]} ; do rm -f ${TESTDIR}/${f}.genozip ; done
    cleanup_cache
}


batch_real_world_with_ref_backcomp()
{
    if [ "$i_am_prod" == "1" ]; then return; fi 

    batch_print_header

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    # with a reference
    local files37=( test.IonXpress.sam.gz \
                    test.human.fq.gz test.human2.bam test.pacbio.clr.bam \
                    test.human2-R1.fq.bz2 test.pacbio.ccs.10k.bam \
                    test.NA12878.chr22.1x.bam test.NA12878-R1.100k.fq  \
                    test.human2.filtered.snp.vcf test.solexa-headerless.sam )

    local files38=( test.1KG-38.vcf.gz test.human-collated-headerless.sam test.cigar-no-seq-qual.bam)

    local filesT2T1_1=( test.nanopore.t2t_v1_1.bam )

    local total=$(( ${#files37[@]} + ${#files38[@]} + ${#filesT2T1_1[@]} ))

    local i=0
    for f in ${files37[@]}; do     
        i=$(( i + 1 ))
        test_header "$f - backward compatability with prod (with reference) - 37 ($i/$total)"
        $genozip_latest $TESTDIR/$f -mf -e $hs37d5 -o $output || exit 1
        $genounzip -t $output || exit 1
    done

    for f in ${files38[@]}; do 
        i=$(( i + 1 ))
        test_header "$f - backward compatability with prod (with reference) - 38 ($i/$total)"
        $genozip_latest $TESTDIR/$f -mf -e $GRCh38 -o $output || exit 1
        $genounzip -t $output || exit 1
    done

    for f in ${filesT2T1_1[@]}; do 
        i=$(( i + 1 ))
        test_header "$f - backward compatability with prod (with reference) - T2T ($i/$total)"
        $genozip_latest $TESTDIR/$f -mf -e $T2T1_1 -o $output || exit 1
        $genounzip -t $output || exit 1
    done
}

batch_real_world_backcomp()
{
    batch_print_header $1

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    # compress all real world test files with old genozip version. Some files might fail compression -
    # they will remain with size 0 and we will ignore them in this test.
    install_license $1 || exit 1
    make -C $TESTDIR $1.version  # generate files listed in "files"
    
    local files=( $TESTDIR/$1/*.genozip )
    
    # remove reference file from list
    files=( ${files[@]/"$TESTDIR/$1/hs37d5.ref.genozip"} ) 

    # bug 1099 (only applicable to 11.0.11 with debug)
    if [[ "$1" == "11.0.11" && "$is_debug" != "" ]]; then
        files=( ${files[@]/"$TESTDIR/11.0.11/test.1KG-37.vcf.genozip"} ) 
    fi

    # remove 0-size files - test/Makefile sets file size to 0, if old Genozip version failed on this (in compression or test)
    for f in ${files[@]}; do     
        if [ ! -s $f ]; then 
            files=( ${files[@]/"$f"} ) 
        fi
    done

    local i=0
    for f in ${files[@]}; do     
        i=$(( i + 1 ))            
        test_header "$f - backcomp with $1 ($i/${#files[@]} batch_id=${GENOZIP_TEST})"
        $genounzip -t $f -e $TESTDIR/$1/hs37d5.ref.genozip || exit 1
    done

    test_header "backcomp $1 genounzip --bgzf=exact"
    local file=test.human2.bam
    $genounzip $TESTDIR/$1/$file.genozip -fo $OUTDIR/$file --bgzf=exact || exit 1
    cmp_2_files_exact $TESTDIR/$file $OUTDIR/$file

    cleanup
}

batch_real_world_small_vbs()
{
    batch_print_header

    cleanup # note: cleanup doesn't affect TESTDIR, but we shall use -f to overwrite any existing genozip files

    # lots of small VBs
    local files=( test.IonXpress.sam.gz                                 \
                  test.human.fq.gz test.human2.bam test.starsolo.sam    \
                  test.human2-R1.fq.bz2 test.pacbio.ccs.10k.bam         \
                  test.pacbio.clr.bam `# multiple PRIM and DEPN vbs`    \
                  test.NA12878.chr22.1x.bam test.NA12878-R1.100k.fq     \
                  test.sequential.fa.gz )

    if [ -x "$(command -v xz)" ] ; then # skip .xz files if xz is not installed
        files+=( test.pacbio.10k.fasta.xz )
    fi

    echo "subsets of real world files (lots of small VBs --vblock=100000B --force-gencomp)"
    test_standard "-mf $1 --vblock=100000B --show-filename --force-gencomp" " " ${files[*]}

    for f in ${files[@]}; do rm -f $TESTDIR/${f}.genozip; done

    # test --pair and --deep with small VBs
    $genozip --vblock=100000B -2tfe $GRCh38 $TESTDIR/test.human2-R1.fq.gz $TESTDIR/test.human2-R2.fq.gz || exit 1 # 2 pairs
    $genozip --vblock=100000B -3tfe $GRCh38 $TESTDIR/deep.human2-38.R1.fq.gz $TESTDIR/deep.human2-38.R2.fq.gz $TESTDIR/deep.human2-38.sam --not-paired || exit 1
    $genozip --vblock=100000B -3tfe $GRCh38 $TESTDIR/deep.bismark.sra2.one.fq.gz $TESTDIR/deep.bismark.sra2.two.fq.gz $TESTDIR/deep.bismark.sra2.bam || exit 1
}

batch_multiseq()
{
    batch_print_header

    # note: not test.virus.nanopore.fq : see bug 917
    local files=( test.coronavirus.fasta test.virus.pacbio-subreads.fq.gz test.virus.iontorrent.fq.gz )

# TODO
    # for f in ${files38[@]}; do 
    #     test_header "$f - checking multiseq identification"
    # done

# OLD
    # test_standard "--multiseq" " " test.coronavirus.fasta

    # # regions
    # test_count_genocat_lines "$TESTDIR/test.coronavirus.fasta" "--regions MW362225.1" 22
    # test_count_genocat_lines "$TESTDIR/test.coronavirus.fasta" "--regions ^MW362225.1" 99978

    # test_standard "--multiseq" " " test.nanopore-virus.fq
}

get_sam_type() # $1 = filename
{
    local first_chars="@RG" # first 3 characters of test.ubam.sam

    local head="`head -c4 $1`"
    if [[ "$head" == "CRAM" ]];               then echo "CRAM";   return; fi
    if [[ "${head:0:3}" == "$first_chars" ]]; then echo "SAM";    return; fi 

    local bhead="`head -c26 $OUTDIR/txt | tail -c3`"
    if [[ "$bhead" == "BAM" ]];               then echo "BAM_Z0"; return; fi # BGZF with non-compressed blocks

    local zhead="`$zcat < $1 2>/dev/null | head -c3`" # zcat of a file called txt doesn't work on mac, hence input redirection
    if [[ "$zhead" == "BAM" ]];               then echo "BAM";    return; fi
    if [[ "$zhead" == "$first_chars" ]];      then echo "SAM_GZ"; return; fi
}

batch_sam_bam_cram_output()
{
    batch_print_header
    if ! `command -v samtools >& /dev/null`; then return; fi

    # verify data types when pizzing a cram file
    local txt=$OUTDIR/txt # no file extension, so that it doesn't modify the data type

    local src=$TESTDIR/test.ubam.bam # small file, but not too small so that there is a difference between -z0 and -z1
    local name=$OUTDIR/test.ubam 

    # prepare files
    samtools view -h $src -OSAM -o $name.sam || exit 1
    gzip -c $name.sam > $name.sam.gz
    cp $src $name.bam
    samtools view $src -OCRAM -o $name.cram >& /dev/null || exit 1

    # tests flags_piz_set_out_dt and mgzip_piz_calculate_mgzip_flags 
    for file in $name.sam $name.sam.gz $name.bam $name.cram; do
        local z=$file.genozip

        $genozip -ft $file -o $z || exit 1
        z_type=$(get_sam_type $file) 

        test_header "#1: test genounzip of `basename $z`: should be the same type"
        $genounzip $z -fo $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != $z_type ]]; then echo "genounzip of $z_type unexpectedly generated $txt_type"; exit 1; fi

        test_header "#2: genocat of `basename $z`: implicit - should be the same type"
        $genocat $z -fo $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != $z_type ]]; then echo "genocat (implicit) of $z_type unexpectedly generated $txt_type"; exit 1; fi

        test_header "#3: genocat of `basename $z`: stdout - should be SAM"
        $genocat $z > $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "SAM" ]]; then echo "genocat (stdout) of $z_type unexpectedly generated $txt_type, expecting SAM"; exit 1; fi

        test_header "#4: genocat of `basename $z`: explicitly SAM by filename"
        $genocat $z -fo $txt.sam || exit 1
        txt_type=$(get_sam_type $txt.sam)
        if [[ $txt_type != "SAM" ]]; then echo "genocat (-o .sam) of $z_type unexpectedly generated $txt_type, expecting SAM"; exit 1; fi

        test_header "#5: genocat of `basename $z`: explicitly SAM by flag"
        $genocat $z -fo $txt --sam || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $z_type == "SAM" && $txt_type != "SAM" ]]; then echo "genocat (--sam) of $z_type unexpectedly generated $txt_type, expecting SAM"; exit 1; fi
        if [[ $z_type != "SAM" && $txt_type != "SAM_GZ" ]]; then echo "genocat (--sam) of $z_type unexpectedly generated $txt_type, expecting SAM_GZ"; exit 1; fi

        test_header "#6: genocat of `basename $z`: stdout as SAM.gz"
        $genocat $z --sam -z1 > $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "SAM_GZ" ]]; then echo "genocat (stdout as SAM.gz) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#7: genocat of `basename $z`: explicitly SAM.gz by .sam.gz filename"
        $genocat $z -fo $txt.sam.gz || exit 1
        txt_type=$(get_sam_type $txt.sam.gz)
        if [[ $txt_type != "SAM_GZ" ]]; then echo "genocat (-o .sam.gz) of $z_type unexpectedly generated $txt_type, expecting SAM.gz"; exit 1; fi

        test_header "#8: genocat of `basename $z`: explicitly SAM.gz by --sam + .gz filename"
        $genocat $z -fo $txt.gz --sam || exit 1
        txt_type=$(get_sam_type $txt.gz)
        if [[ $txt_type != "SAM_GZ" ]]; then echo "genocat (--sam -o .gz) of $z_type unexpectedly generated $txt_type, expecting SAM.gz"; exit 1; fi

        test_header "#9: genocat of `basename $z`: stdout as BAM"
        $genocat $z --bam -z1 > $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "BAM" ]]; then echo "genocat (stdout as BAM) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#10: genocat of `basename $z`: explicitly BAM by filename"
        $genocat $z -fo $txt.bam || exit 1
        txt_type=$(get_sam_type $txt.bam)
        if [[ $txt_type != "BAM" ]]; then echo "genocat (-o .bam) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#11: genocat of `basename $z`: explicitly BAM by flag"
        $genocat $z -fo $txt --bam || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "BAM" ]]; then echo "genocat (--bam) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#12: genocat of `basename $z`: stdout as BAM_Z0"
        $genocat $z --bam > $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "BAM_Z0" ]]; then echo "genocat (stdout as BAM_Z0) of $z_type unexpectedly generated $txt_type, expecting BAM_Z0"; exit 1; fi

        test_header "#13: genocat of `basename $z`: explicitly BAM_Z0 by filename"
        $genocat $z -fo $txt.bam -z0 || exit 1
        txt_type=$(get_sam_type $txt.bam)
        if [[ $txt_type != "BAM_Z0" ]]; then echo "genocat (-o .bam -z0) of $z_type unexpectedly generated $txt_type, expecting BAM_Z0"; exit 1; fi

        test_header "#14: genocat of `basename $z`: explicitly BAM_Z0 by flag"
        $genocat $z -fo $txt --bam -z0 || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "BAM_Z0" ]]; then echo "genocat (--bam -z0) of $z_type unexpectedly generated $txt_type, expecting BAM_Z0"; exit 1; fi

        test_header "#15: genocat of `basename $z`: stdout as CRAM"
        $genocat $z --cram > $txt || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "CRAM" ]]; then echo "genocat (stdout as CRAM) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#16: genocat of `basename $z`: explicitly CRAM by filename"
        $genocat $z -fo $txt.cram || exit 1
        txt_type=$(get_sam_type $txt.cram)
        if [[ $txt_type != "CRAM" ]]; then echo "genocat (-o .cram) of $z_type unexpectedly generated $txt_type, expecting CRAM"; exit 1; fi

        test_header "#17: genocat of `basename $z`: explicitly CRAM by flag"
        $genocat $z -fo $txt --cram || exit 1
        txt_type=$(get_sam_type $txt)
        if [[ $txt_type != "CRAM" ]]; then echo "genocat (--cram) of $z_type unexpectedly generated $txt_type, expecting CRAM"; exit 1; fi
    done

    cleanup
}

get_vcf_type() # $1 = filename
{
    local first_chars="##f" # first 3 characters of test.svaba.somatic.sv.vcf

    local head="`head -c3 $1`"
    if [[ "$head" == "$first_chars" ]];  then echo "VCF";    return; fi

    local zhead="`$zcat < $1 2>/dev/null | head -c3`" # zcat of a file called txt doesn't work on mac, hence input redirection
    if [[ "$zhead" == "BCF" ]];          then echo "BCF";    return; fi
    if [[ "$zhead" == "$first_chars" ]]; then echo "VCF_GZ"; return; fi
}

batch_vcf_bcf_output()
{
    batch_print_header
    if ! `command -v bcftools >& /dev/null`; then return; fi

    # verify data types when pizzing a cram file
    local txt=$OUTDIR/txt # no file extension, so that it doesn't modify the data type

    local src=$TESTDIR/test.svaba.somatic.sv.vcf  # small file, but not too small so that there is a difference between -z0 and -z1
    local name=$OUTDIR/test.svaba.somatic.sv

    # prepare files
    cp $src $name.vcf
    bcftools view -h $src -Oz1 -o $name.vcf.gz >& /dev/null || exit 1
    bcftools view -h $src -Ob  -o $name.bcf    >& /dev/null || exit 1

    # tests flags_piz_set_out_dt and mgzip_piz_calculate_mgzip_flags 
    for file in $name.vcf $name.vcf.gz $name.bcf ; do
        local z=$file.genozip

        $genozip -ft $file -o $z || exit 1
        z_type=$(get_vcf_type $file) 

        test_header "#1: test genounzip of `basename $z`: should be the same type"
        $genounzip $z -fo $txt || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != $z_type ]]; then echo "genounzip of $z_type unexpectedly generated $txt_type"; exit 1; fi

        test_header "#2: genocat of `basename $z`: implicit - should be the same type"
        $genocat $z -fo $txt || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != $z_type ]]; then echo "genocat (implicit) of $z_type unexpectedly generated $txt_type"; exit 1; fi

        test_header "#3: genocat of `basename $z`: stdout - should be VCF"
        $genocat $z > $txt || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != "VCF" ]]; then echo "genocat (stdout) of $z_type unexpectedly generated $txt_type, expecting VCF"; exit 1; fi

        test_header "#4: genocat of `basename $z`: explicitly VCF by filename"
        $genocat $z -fo $txt.vcf || exit 1
        txt_type=$(get_vcf_type $txt.vcf)
        if [[ $txt_type != "VCF" ]]; then echo "genocat (-o .vcf) of $z_type unexpectedly generated $txt_type, expecting VCF"; exit 1; fi

        test_header "#5: genocat of `basename $z`: explicitly VCF by flag"
        $genocat $z -fo $txt --vcf || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $z_type == "VCF" && $txt_type != "VCF"    ]]; then echo "genocat (--vcf) of $z_type unexpectedly generated $txt_type, expecting VCF"; exit 1; fi
        if [[ $z_type != "VCF" && $txt_type != "VCF_GZ" ]]; then echo "genocat (--vcf) of $z_type unexpectedly generated $txt_type, expecting VCF_GZ"; exit 1; fi

        test_header "#6: genocat of `basename $z`: stdout as VCF.gz"
        $genocat $z --vcf -z1 > $txt || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != "VCF_GZ" ]]; then echo "genocat (stdout as VCF.gz) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#7: genocat of `basename $z`: explicitly VCF.gz by .vcf.gz filename"
        $genocat $z -fo $txt.vcf.gz || exit 1
        txt_type=$(get_vcf_type $txt.vcf.gz)
        if [[ $txt_type != "VCF_GZ" ]]; then echo "genocat (-o .vcf.gz) of $z_type unexpectedly generated $txt_type, expecting VCF.gz"; exit 1; fi

        test_header "#8: genocat of `basename $z`: explicitly VCF.gz by --vcf + .gz filename"
        $genocat $z -fo $txt.gz --vcf || exit 1
        txt_type=$(get_vcf_type $txt.gz)
        if [[ $txt_type != "VCF_GZ" ]]; then echo "genocat (--vcf -o .gz) of $z_type unexpectedly generated $txt_type, expecting SAM.gz"; exit 1; fi

        test_header "#9: genocat of `basename $z`: stdout as BCF"
        $genocat $z --bcf > $txt || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != "BCF" ]]; then echo "genocat (stdout as BCF) of $z_type unexpectedly generated $txt_type, expecting BAM"; exit 1; fi

        test_header "#10: genocat of `basename $z`: explicitly BCF by filename"
        $genocat $z -fo $txt.bcf || exit 1
        txt_type=$(get_vcf_type $txt.bcf)
        if [[ $txt_type != "BCF" ]]; then echo "genocat (-o .bcf) of $z_type unexpectedly generated $txt_type, expecting BCF"; exit 1; fi

        test_header "#11: genocat of `basename $z`: explicitly BCF by flag"
        $genocat $z -fo $txt --bcf || exit 1
        txt_type=$(get_vcf_type $txt)
        if [[ $txt_type != "BCF" ]]; then echo "genocat (--bcf) of $z_type unexpectedly generated $txt_type, expecting BCF"; exit 1; fi
    done

    cleanup
}

# unzip
batch_external_unzip()
{
    batch_print_header
    if `command -v unzip >& /dev/null`; then
        test_standard " " " " test.genome_Full.zip    
    fi

    cleanup
}

# ORA
batch_external_ora()
{
    batch_print_header

    return; # skipped because its very slow - 80 seconds.
    # Reason for slowness: test.fastq.ora is a abrupt subset of a larger file - orad doesn't like that.
    
    if `command -v orad >& /dev/null`; then
        ORA_REF_PATH=$REFDIR $genozip --truncate -ft -e $hs37d5 $TESTDIR/test.fastq.ora || exit $?
    fi

    cleanup
}

batch_reference_fastq()
{
    batch_print_header

    echo "paired FASTQ with --reference, --password (BZ2), --md5, -B1"
    test_standard "CONCAT -e$GRCh38 -p 123 --pair -mB1" "-p123" test.human2-R1.fq.bz2 test.human2-R2.fq.bz2

    # test --grep (regression test for bug 788)
    local n
    n=`$genocat --grep "@A00910:85:HYGWJDSXX:1:1101:9028:1000" -p 123 $output --count` || exit $?
    if (( n != 2 )); then
        echo "Expecting 2 reads to be grepped in paired FASTQ"
        exit 1
    fi

    # test single-line --head (only pair-1 is expressed) - note: cannot use --count with --head
    n=`$genocat --head=1 -p 123 $output | wc -l` || exit $? 
    if (( n != 4 )); then
        echo "Expecting 1 read to be counted with --head=1 in paired FASTQ but lines=$n"
        exit 1
    fi

    # test single-line --tail (only pair-2 expressed) - note: cannot use --count with --tail
    n=`$genocat --tail=1 -p 123 $output | wc -l` || exit $? 
    if (( n != 4 )); then
        echo "Expecting 1 reads to be counted with --tail=1 in paired FASTQ"
        exit 1
    fi

    # test --bases
    n=`$genocat --bases=N -p 123 $output --count || exit 1` || exit $? 
    if (( n != 99 )); then
        echo "Expecting 99 reads to be counted with --bases=N in this paired FASTQ"
        exit 1
    fi

    echo "4 paired FASTQ with --REFERENCE (BGZF, decompress concatenated, password)"
    test_standard "COPY -E$GRCh38 -2 -p 123" " " test.human2-R1.fq.gz test.human2-R2.fq.gz
    
    # solexa read style
    test_standard "-e$GRCh38 --pair" "" special.solexa-R1.fq special.solexa-R2.fq

    cleanup
}

batch_reference_fasta_as_fastq()
{
    batch_print_header

    echo "FASTA with --reference, --password, --md5, -B1"
    test_standard "-e$GRCh38 -p 123 -mB1" "-p123" test.human2-R1.fa.gz

    # test --grep (regression test for bug 788)
    local n
    n=`$genocat --grep "@A00910:85:HYGWJDSXX:1:1101:9028:1000" -p 123 $output --count` || exit $?
    if (( n != 1 )); then
        echo "Expecting 1 reads to be grepped in FASTA-as-FASTQ"
        exit 1
    fi

    # test single-line --head (only pair-1 is expressed) - note: cannot use --count with --head
    n=`$genocat --head=1 -p 123 $output | wc -l` || exit $? 
    if (( n != 2 )); then
        echo "Expecting 1 read to be counted with --head=1 in FASTA-as-FASTQ but lines=$n"
        exit 1
    fi

    # test single-line --tail (only pair-2 expressed) - note: cannot use --count with --tail
    n=`$genocat --tail=1 -p 123 $output | wc -l` || exit $? 
    if (( n != 2 )); then
        echo "Expecting 1 reads to be counted with --tail=1 in FASTA-as-FASTQ"
        exit 1
    fi

    # test --bases
    n=`$genocat --bases=N -p 123 $output --count || exit 1` || exit $? 
    if (( n != 49 )); then
        echo "Expecting 49 reads to be counted with --bases=N in this FASTA-as-FASTQ"
        exit 1
    fi

    # test --header-only
    n=`$genocat --header-only -p 123 $output | wc -l` || exit $? 
    if (( n != 100000 )); then
        echo "Expecting 100000 lines to be counted with -header-only in FASTA-as-FASTQ"
        exit 1
    fi

    # test --seq-only
    n=`$genocat --seq-only -p 123 $output | wc -l` || exit $? 
    if (( n != 100000 )); then
        echo "Expecting 100000 lines to be counted with --seq-only in FASTA-as-FASTQ"
        exit 1
    fi

    echo "2 FASTA with --REFERENCE (BGZF, decompress concatenated, password)"
    test_standard "COPY -E$GRCh38 -p 123" " " test.human2-R1.fa.gz
    
    cleanup
}

batch_reference_sam()
{
    batch_print_header

    cleanup_cache
    echo "command line with mixed SAM and FASTQ files with --reference"
    echo "Note: '$GRCh38' needs to be up to date with the latest genozip format"
    test_standard "-me$GRCh38" " " test.human.fq.gz test.human3-collated.bam

    echo "multiple SAM with --REFERENCE" 
    test_standard "-mE$GRCh38" " " test.human-collated-headerless.sam test.human3-collated.bam
    
    echo "SAM with --REFERENCE and --password" 
    test_standard "-E$GRCh38 --password 123" "-p123" test.human-collated-headerless.sam
    cleanup_cache

    cleanup_cache
    echo "BAM with --reference and --password, alternate chrom names" 
    test_standard "-me$hg19_plusMT --password 123" "-p123 -e$hg19_plusMT" test.human2.bam  
    cleanup_cache

    cleanup_cache
    echo "SAM with large (>4GB) plant genome"  
    test_standard "-me$chinese_spring --password 123" "-p123 -e$chinese_spring" test.bsseeker2-wgbs.sam.gz  

    cleanup
}

batch_reference_vcf()
{
    batch_print_header

    echo "multiple VCF with --reference, --md5 using hs37d5 ; alternate chroms names"
    test_standard "COPY -me$hg19_plusMT" " " test.human2.filtered.snp.vcf
    cleanup_cache

    echo "GVCF with --reference, --md5 using GRCh38"
    test_standard "-me$GRCh38" " " test.g.vcf.gz
    cleanup_cache

    echo "multiple VCF with --REFERENCE using hs37d5, password" 
    test_standard "-mE$hs37d5 -p123" "--password 123" test.1KG-37.vcf test.human2.filtered.snp.vcf

    cleanup
}

batch_many_small_files()
{
    batch_print_header

    cleanup

    local num_files=300
    echo "Generating $num_files small files"
    mkdir ${OUTDIR}/smalls
    for i in `seq $num_files`; do cp ${TESTDIR}/minimal.sam ${OUTDIR}/smalls/minimal.${i}.sam; done

    $genozip -ft -D ${OUTDIR}/smalls || exit 1

    cleanup
}

batch_make_reference()
{
    batch_print_header

    cleanup

    local fa_file=$REFDIR/GRCh38_full_analysis_set_plus_decoy_hla.fa.gz 
    local ref_file=$OUTDIR/output.ref.genozip

    # test making from a URL
    echo "Making a reference from a URL: $genozip --make-reference file://$path$fa_file"
    $genozip --make-reference file://$path$fa_file --force -o $ref_file || exit 1

    # test making from stdin
    test_header "Making a reference from stdin: $genozip --make-reference -fo $ref_file - < $fa_file"
    $genozip --make-reference -fo $ref_file - < $fa_file || exit 1

    # Making a reference from a local file
    test_header "Making a reference for a local file: $genozip --make-reference $fa_file"
    $genozip --make-reference $fa_file --force -o $ref_file || exit 1

    local ref="--reference $ref_file"
    local REF="--REFERENCE $ref_file"

    echo "unaligned SAM with --reference"
    test_standard "$ref" "$ref" basic-unaligned.sam 

    echo "unaligned SAM with --REFERENCE"
    test_standard "$REF" " " basic-unaligned.sam

    echo "unalignable SAM with --REFERENCE"
    test_standard "$REF" " " basic-unalignable.sam

    echo "unaligned BAM with --reference"
    test_standard "$ref" "$ref" basic-unaligned.bam

    echo "FASTQ with --REFERENCE"
    test_standard "$REF" " " basic.fq 

    echo "unaligned SAM with --REFERENCE - from stdin"
    test_redirected basic-unaligned.sam "$REF"

    # test using env var $GENOZIP_REFERENCE
    if [ -n "$is_windows" ]; then cleanup_cache; fi # in Windows, we have to clear the cache before mv, bc holder process has a open handle to the file
    local alt_ref_file_name=$OUTDIR/output2.ref.genozip
    mv $ref_file $alt_ref_file_name || exit 1
    $genozip $TESTDIR/basic.fq -e $alt_ref_file_name -fXo $output || exit 1

    if [ -n "$is_windows" ]; then cleanup_cache; fi 
    mv $alt_ref_file_name $ref_file || exit 1
    export GENOZIP_REFERENCE=${ref_file}
    $genounzip -t $output || exit 1 # alt_ref_file_name no longer exists, so genounzip depends on GENOZIP_REFERENCE
    
    # partial cleanup - keep reference as we need it for batch_reference_backcomp
    unset GENOZIP_REFERENCE
    rm -f $output
    cleanup_cache
}

update_latest()
{
    if [ ! -d ../genozip-latest ]; then
        echo "can't find ../genozip-latest"
        exit 1
    fi

    pushd ../genozip-latest
    git reset --hard
    git pull

    if [ -n "$is_mac" ]; then 
        chmod +x src/*.sh # reverted by git pull
    fi  
    
    make -j clean
    make -j 
    popd
}

# compress with prod and prod-created ref file, uncompress with new version and new version-created ref file
# ref files are expected have the same MD5
batch_reference_backcomp()
{
    batch_print_header    
    cleanup_cache

    local fa_file=$REFDIR/GRCh38_full_analysis_set_plus_decoy_hla.fa.gz 
    local ref_file=$OUTDIR/output.ref.genozip
    local prod_ref_file=$OUTDIR/output.prod.ref.genozip

    # new ref file normally created by batch_make_reference, but we create if for some reason its not
    if [ ! -f $ref_file ]; then
        echo "Making reference"
        $genozip --make-reference $fa_file --force -o $ref_file || exit 1
    fi

    update_latest

    if [ ! -f $prod_ref_file ]; then
        echo "Making prod reference"
        $genozip_latest --make-reference $fa_file --force -o $prod_ref_file || exit 1
    fi

    local files38=( test.human.fq.gz test.human-collated-headerless.sam test.1KG-38.vcf.gz )

    for f in ${files38[@]}; do 
        test_header "$f - reference file backward compatability with prod"

        echo "old file, old reference, new genounzip"
        $genozip_latest $TESTDIR/$f -mf -e $prod_ref_file -o $output -X || exit 1
        $genounzip -t $output -e $prod_ref_file || exit 1

        local latest_version
        latest_version=`$genozip_latest -V | cut -c9-10` || exit $? 
        if (( latest_version >= 15 )); then # prior to v15 we didn't in have the in-memory digest
            echo "old file, old reference, new genounzip with new reference"
            $genounzip -t $output -e $ref_file || exit 1
        fi

        echo "new file, old reference, new genounzip"
        $genozip $TESTDIR/$f -mft -e $prod_ref_file -o $output || exit 1
    done

    cleanup
}

# compress headerless SAM with wrong ref. Expected to work (with bad compression ratio)
batch_headerless_wrong_ref()
{
    batch_print_header

    $genozip -ft ${TESTDIR}/test.human-collated-headerless.sam -e $hs37d5 || exit 1 
    
    cleanup
}

test_exists()
{
    if [ ! -f $1 ]; then echo "Expecting $1 to exist, but it doesn't" ; exit 1 ; fi
}

test_not_exists()
{
    if [ -f $1 ]; then echo "Expecting $1 to not exist, but it does" ; exit 1 ; fi
}

# test that --replace (or -^) replaces and without --replace doesn't
batch_replace()
{
    batch_print_header

    local f1=${OUTDIR}/f1.fq
    local f2=${OUTDIR}/f2.fq
    local f3=${OUTDIR}/f3.sam

    # single file - genozip
    test_header "batch replace: single file - genozip"
    cp ${TESTDIR}/basic.fq $f1
    $genozip $f1 -fX || exit 1
    test_exists $f1

    $genozip $f1 -ft --replace || exit 1
    test_not_exists $f1

    # single file - genounzip
    test_header "batch replace: single file - genounzip"
    $genounzip -f $f1.genozip || exit 1
    test_exists $f1.genozip

    $genounzip -f $f1.genozip --replace --test # expecting failure and no replacement
    verify_failure genounzip $?
    test_exists $f1.genozip

    if [ -n "$is_windows" ]; then
        sleep 1 # windows: without this, DeleteFile of genozip file sometimes fails as "file in use"
    fi

    $genounzip -f $f1.genozip -^ || exit 1
    test_not_exists $f1.genozip

    # multiple files
    test_header "batch replace: multiple files"
    cp ${TESTDIR}/basic.fq $f1
    cp ${TESTDIR}/basic.fq $f2
    
    $genozip -fX $f1 $f2 || exit 1
    test_exists $f1
    test_exists $f2

    $genozip $f1 $f2 -ft -^ || exit 1
    test_not_exists $f1
    test_not_exists $f2

    $genounzip -f $f1.genozip $f2.genozip || exit 1
    test_exists $f1.genozip
    test_exists $f2.genozip

    usleep 500000
    $genounzip -f $f1.genozip $f2.genozip -^ || exit 1
    test_not_exists $f1.genozip
    test_not_exists $f2.genozip

    # paired
    test_header "batch replace: paired"
    cp ${TESTDIR}/basic-deep.R1.fq $f1
    cp ${TESTDIR}/basic-deep.R2.fq $f2
    
    $genozip -fX $f1 $f2 -2e $hs37d5 -o $output || exit 1
    test_exists $f1
    test_exists $f2

    $genozip $f1 $f2 -ft^2e $hs37d5 -o $output || exit 1
    test_not_exists $f1
    test_not_exists $f2

    $genounzip -f $output || exit 1
    test_exists $output

    $genounzip -f $output -^ || exit 1
    test_not_exists $output

    # deep
    test_header "batch replace: deep"
    cp ${TESTDIR}/basic-deep.R1.fq $f1
    cp ${TESTDIR}/basic-deep.R2.fq $f2
    cp ${TESTDIR}/basic-deep.sam   $f3

    $genozip $f1 $f2 $f3 -fX3e $GRCh38 -o $output || exit 1
    test_exists $f1
    test_exists $f2
    test_exists $f3

    $genozip $f1 $f2 $f3 -^ft3e $GRCh38 -o $output || exit 1
    test_not_exists $f1
    test_not_exists $f2
    test_not_exists $f3

    $genounzip -f $output || exit 1
    test_exists $output

    $genounzip -f $output --replace || exit 1
    test_not_exists $output
}

batch_genols()
{
    batch_print_header

    $genozip ${TESTDIR}/basic.fq ${TESTDIR}/basic.fq -2 -e $GRCh38 -Xfo $output -p abcd || exit 1
    $genols $output -p abcd || exit 1
    rm -f $output
}

batch_tar_files_from()
{
    batch_print_header

    cleanup

    test_header "genozip --tar + --subdirs + --files-from"

    local tar=${OUTDIR}/output.tar
    pushd $TESTDIR/../.. # avoid problems with relative paths in basic-files-from* if testing on Windows
    
    $genozip -D -T ${TESTDIR}/basic-files-from -Xf --tar $tar || exit 1 
    
    test_header "genols ; genocat --files-from"

    tar xvf $tar || exit 1
    cat ${TESTDIR}/basic-files-from-genozip | $genounzip --files-from - -t || exit 1
    
    $genols --files-from ${TESTDIR}/basic-files-from-genozip || exit 1
    $genocat --files-from ${TESTDIR}/basic-files-from-genozip > $output || exit 1

    test_header "genozip --tar + --subdirs + --files-from + --reference + test"
    
    $genozip -D -T ${TESTDIR}/basic-files-from -tf --tar $tar --reference $hs37d5  || exit 1

    popd
    
    cleanup
}

batch_gencomp_depn_methods() # note: use --debug-gencomp for detailed tracking
{
    batch_print_header

    # invoke REREAD with plain file (test.pacbio.clr.bam is NOT compressed with BGZF)
    # -B1 forces multiple depn VBs
    # -@3 sets DEPN queue length to 3 forcing other VBs to be REREAD
    $genozip -fB1 -@3 -t $TESTDIR/test.pacbio.clr.bam --force-gencomp || exit 1 

    # invoke REREAD with BGZF file (test.pacbio.clr.bam.gz is generated by test/Makefile)
    $genozip -fB1 -@3 -t $TESTDIR/test.pacbio.clr.bam.gz --force-gencomp || exit 1

    # invoke OFFLOAD method
    $genozip -fB1 -@3 -t --force-gencomp file://${path}${TESTDIR}/test.pacbio.clr.bam -fo $output || exit 1
    cat $TESTDIR/test.pacbio.clr.bam.gz | $genozip -i bam -fB1 -@3 -t - -fo $output || exit 1

    # SAG by SA
    $genozip -fB1 -@3 -t $TESTDIR/special.sag-by-sa.sam --force-gencomp || exit 1

    # SAG by CC
    $genozip -fB1 -@3 -t $TESTDIR/test.sag-by-cc.bam --force-gencomp || exit 1

    # Alignments with and without CIGAR
    $genozip -fB1 -@3 -t $TESTDIR/test.saggy-alns-with-and-without-CIGAR.sam.gz --force-gencomp || exit 1

    # Missing QUAL for depn
    $genozip -fB1 -@3 -t $TESTDIR/test.nanopore-minimap2-longr-depn_no_qual.bam --force-gencomp || exit 1

    cleanup
}

batch_deep() # note: use --debug-deep for detailed tracking
{
    batch_print_header

    # btest contains a variety of scenarios
    test_header basic-deep
    local T=$TESTDIR/basic-deep
    $genozip $T.sam $T.R1.fq $T.R2.fq -tfe $GRCh38 --deep -o $output --best || exit 1 # --best causes aligner use on unmapped alignments
    $genozip $T.sam $T.R1.fq $T.R2.fq -tfe $GRCh38 --deep -o $output --no-gencomp || exit 1 # --no-gencomp causes in-VB segging against saggy 
    $genozip $T.sam $T.R1.fq $T.R2.fq -tfe $GRCh38 --deep -o $output --md5 || exit 1 # --md5 uses a differt code path for verifying digest

    test_count_genocat_lines "" "--R1" 24
    test_count_genocat_lines "" "--R2" 24
    test_count_genocat_lines "" "--interleave" 48
    test_count_genocat_lines "" "--sam --no-header" 13
    test_count_genocat_lines "" "--sam --header-only" 3376
     
    test_header deep.human2-38
    local T=$TESTDIR/deep.human2-38
    $genozip $T.sam $T.R1.fq.gz $T.R2.fq.gz -fe $GRCh38 -o $output -3t --best --not-paired || exit 1
    $genozip $T.sam $T.R1.fq.gz $T.R2.fq.gz -fe $GRCh38 -o $output -3t --no-gencomp --not-paired || exit 1

    # bismark (bisulfite), SRA2, non-matching FASTQ filenames
    test_header deep.bismark.sra2
    local T=$TESTDIR/deep.bismark.sra2
    $genozip $T.two.fq.gz $T.bam $T.one.fq.gz -fE $GRCh38 -o $output -3t || exit 1
    
    # gem3 (bisulfite), multiple (>2) FASTQ
    test_header deep.gem3.multi-fastq
    local T=$TESTDIR/deep.gem3.multi-fastq
    $genozip $T.1.fq $T.sam $T.2.fq $T.3.fq -fe $GRCh38 -o $output -3t || exit 1
    test_count_genocat_lines "" "--R 1" 20
    test_count_genocat_lines "" "--R 2" 20
    test_count_genocat_lines "" "--R 3" 20

    # SAM has cropped one base at the end of every read (101 bases in FQ vs 100 in SAM)
    test_header deep.crop-100
    local T=$TESTDIR/deep.crop-100
    $genozip $T.fq $T.sam -fe $GRCh38 -o $output -3t || exit 1
    test_count_genocat_lines "" "--fq --seq-only" 1000
    test_count_genocat_lines "" "--fq --qual-only" 1000
    # test_count_genocat_lines "" "--fq --header-only" 1000 # bug 857

    # SAM and FQ qname flavor is different - but comparable after canonization
    test_header deep.canonize-qname
    local T=$TESTDIR/deep.canonize-qname
    $genozip $T.2.fq $T.1.fq $T.sam -tfe $GRCh38 -o $output --deep || exit 1

    # SAM sequences may be shorter than in FASTQ due to trimming
    test_header deep.trimmed
    local T=$TESTDIR/deep.trimmed
    $genozip $T.fq $T.sam -fe $GRCh38 -o $output -3t || exit 1

    # trimmed with LONG (codec consumes trimmed SEQ)
    cleanup_cache
    test_header deep.trim+longr
    local T=$TESTDIR/deep.trim+longr
    $genozip $T.fq $T.sam -fe $hs37d5 -o $output -3t || exit 1

    # trimmed with HOMP (codec consumes trimmed SEQ and calls fastq_zip_qual for sub-codec too)
    test_header deep.trim+homp
    local T=$TESTDIR/deep.trim+homp
    $genozip $T.fq $T.sam -fe $hs37d5 -o $output -3t || exit 1

    # FASTQ with SAUX
    test_header deep.illum.saux
    local T=$TESTDIR/deep.illum.saux
    $genozip $T.R1.fq $T.R2.fq $T.sam -tfe $hs37d5 -o $output --deep || exit 1

    # qual scores corresponding to 'N' bases are replaced by DRAGEN
    test_header deep.rewrite-N-qual
    local T=$TESTDIR/deep.rewrite-N-qual
    $genozip $T.R1.fq $T.R2.fq $T.sam -fte $hs37d5 -o $output --deep || exit 1

    if (( `$genozip $T.R1.fq $T.R2.fq $T.sam -fe $hs37d5 -o $output -3X --show-deep | grep "n_full_mch=(24,0)" | wc -l` != 1 )); then
        echo "expecting 24 full matches (including QUAL matches)"
        exit 1
    fi
    
    # Illumina WGS - different FASTQ and SAM qname flavors
    cleanup_cache
    test_header "deep.qtype=QNAME2 - different FASTQ and SAM qname flavors"
    local T="$TESTDIR/deep.qtype=QNAME2"
    $genozip $T.1.fq $T.2.fq $T.sam -tfE $hg19 --deep -o $output || exit 1

    test_header "deep.trimmed-deep_no_qual - encrypted"
    local T=$TESTDIR/deep.trimmed-deep_no_qual
    $genozip $T.bam $T.R1.fq.gz $T.R2.fq.gz -fe $hg19 -p 123 -o $output -3t || exit 1

    test_header "deep.left-right-trimming"
    local T=$TESTDIR/deep.left-right-trimming
    $genozip $T.bam $T.fq.gz -fe $hg19 -o $output -3t || exit 1

    # pacbio ccs, minimap2, single FASTQ
    cleanup_cache
    test_header deep.pacbio-ccs
    local T=$TESTDIR/deep.pacbio-ccs
    $genozip $T.fq.gz $T.bam -fe $mm10 -3t -o $output || exit 1
    
    cleanup
}

# only if doing a full test (starting from 0) - delete genome and hash caches
sparkling_clean()
{
    batch_print_header
    rm -f ${hg19}.*cache* ${hs37d5}.*cache* ${GRCh38}.*cache* ${TESTDIR}/*.genozip ${TESTDIR}/*.bad ${TESTDIR}/*.bad.gz ${TESTDIR}/basic-subdirs/*.genozip ${TESTDIR}/*rejects* ${TESTDIR}/*.DEPN
    unset GENOZIP_REFERENCE
}

set -o pipefail # if any command in a pipe fails, then the pipe exit code is failure 

start_date="`date`"
is_windows="`uname|grep -i mingw``uname|grep -i MSYS`"
is_mac=`uname|grep -i Darwin`
is_linux=`uname|grep -i Linux`

unset GENOZIP_REFERENCE

if [ -n "$is_windows" ] || [ -n "$is_exe" ]; then
BASEDIR=../genozip
else
BASEDIR=.
fi

if [[ ! -v GENOZIP_HOME ]]; then
    export GENOZIP_HOME=$BASEDIR
fi

TESTDIR=$BASEDIR/private/test
SCRIPTSDIR=$BASEDIR/private/scripts
LICENSESDIR=$BASEDIR/private/licenses
OUTDIR=$TESTDIR/tmp
REFDIR=$BASEDIR/public

if [ -n "$is_windows" ]; then
    if [[ ! -v APPDATA ]]; then
        export APPDATA="$BASEDIR/../AppData/Roaming"
    fi

    LICFILE=$APPDATA/genozip/.genozip_license.v15
else
    LICFILE=$HOME/.genozip_license.v15
fi

if [ -n "$is_mac" ]; then 
    chmod +x $BASEDIR/private/scripts/* $BASEDIR/private/utils/mac/* $BASEDIR/src/*.sh # reverted by git pull
fi  

output=${OUTDIR}/output.genozip
output2=${OUTDIR}/output2.genozip
recon=${OUTDIR}/recon.txt

# reference files
hg19=$REFDIR/hg19.v15.ref.genozip
hg19_plusMT=$REFDIR/hg19_plusMT.v15.ref.genozip
hs37d5=$REFDIR/hs37d5.v15.ref.genozip
GRCh38=$REFDIR/GRCh38.v15.ref.genozip
T2T1_1=$REFDIR/chm13_1.1.v15.ref.genozip
mm10=$REFDIR/mm10.v15.ref.genozip
chinese_spring=$REFDIR/Chinese_Spring.v15.ref.genozip

zmd5=$SCRIPTSDIR/zmd5

if (( $# < 1 )); then
    echo "Usage: test.sh [debug|opt|prod] <GENOZIP_TEST-test> [optional-genozip-arg]"
    exit 0
fi

# debug, opt, prod, exe
is_exe=`echo $1|grep exe`
if [ -n "$is_exe" ]; then 
    shift
fi

is_debug=`echo $1|grep debug`
if [ -n "$is_debug" ]; then 
    debug=-debug
    shift
fi

is_opt=`echo $1|grep opt`
if [ -n "$is_opt" ]; then 
    debug=-opt
    shift
fi

# test prod (for a maintainence release)
is_prod=`echo $1|grep prod`
if [ -n "$is_prod" ]; then 
    dir=../genozip-prod/src
    shift
fi

is_conda=`echo $1|grep conda`
if [ -n "$is_conda" ]; then 
    dir=$CONDA_PREFIX/bin
    shift
fi

if [ ! -n "$dir" ]; then 
    dir=$PWD
fi

if (( `pwd | grep genozip-prod | wc -l` == 1 )); then
    i_am_prod=1;
fi

# -----------------
# platform settings
# -----------------
if [ -n "$is_windows" ]; then
    exe=.exe
    path=`pwd| cut -c3-|tr / '\\\\'`\\

elif [ -n "$is_exe" ]; then
    exe=.exe
    path=$PWD/

#    zip_threads="-@3"
#    piz_threads="-@5"
elif [ -n "$is_mac" ]; then
#    exe=.mac
    path=$PWD/
else # linux
    exe=""
    path=$PWD/
fi

export GENOZIP_TEST=$1
shift

# executables
genozip_exe=$dir/genozip${debug}$exe
genozip_latest_exe=../genozip-latest/genozip$exe
genounzip_exe=$dir/genounzip${debug}$exe
genocat_exe=$dir/genocat${debug}$exe
genols_exe=$dir/genols${debug}$exe

# extra args: $1 - zip args $2 - piz args: 
# example: test.sh debug 0 "--no-longer -w" "-z3" 
genozip="$genozip_exe --echo $1 $zip_threads"
genozip_latest="$genozip_latest_exe --echo $1 $zip_threads"
genounzip="$genounzip_exe --echo $2 $piz_threads"
genocat_no_echo="$genocat_exe $2 $piz_threads"
genocat="$genocat_exe --echo $2 $piz_threads"
genols=$genols_exe 

basics=(basic.vcf basic.bcf basic.sam basic.bam basic.fq basic.fa basic.gvf basic.gtf basic.me23 \
        basic.locs basic.bed basic.generic)

exes=($genozip_exe $genounzip_exe $genocat_exe $genols_exe)
for exe in ${exes[@]}; do
    if [ ! -x $exe ]; then
        echo "Error: $exe does not exist"
        exit 1
    fi
done

if [ -n "$is_mac" ]; then
    md5="md5 -q" 
    zcat="gzip -dc"
else
    md5=md5sum 
    zcat=zcat
fi

mkdir $OUTDIR >& /dev/null
cleanup

make -C $TESTDIR --quiet sync_wsl_clock generated md5s || exit 1 # limit threads to 20, otherwise too many concurrent forks (Mac runs out of resources)

# recalculate defective .md5 files
if (( `ls -l $TESTDIR/*.md5 | grep -v 33 | wc -l` != 0 )); then # .md5 files are expected to be size 33
    rm `ls -l private/test/*.md5 | grep -v 33 | rev | cut -d" " -f1 | rev`
    make -C $TESTDIR --quiet md5s # redo
fi

inc() { 
    GENOZIP_TEST=$((GENOZIP_TEST + 1)) 
}

for GENOZIP_TEST in `seq $GENOZIP_TEST 200`; do 
case $GENOZIP_TEST in
0 )  sparkling_clean                   ;;
1 )  batch_minimal                     ;;
2 )  batch_basic basic.vcf             ;;
3 )  batch_basic basic.bam             ;;
4 )  batch_basic basic.sam             ;;
5 )  batch_basic basic.fq              ;;
6 )  batch_basic basic.fa              ;;
7 )  batch_basic basic.bed             ;;
8 )  batch_basic basic.gvf             ;;
9 )  batch_basic basic.gtf             ;;
10)  batch_basic basic.me23            ;;
11)  batch_basic basic.generic         ;;
12)  batch_precompressed               ;;
13)  batch_bgzf                        ;;
14)  batch_mgzip_fastq                 ;;
15)  batch_subdirs                     ;;
16)  batch_special_algs                ;;
17)  batch_qual_codecs                 ;;
18)  batch_sam_bam_translations        ;;
19)  batch_23andMe_translations        ;;
20)  batch_genocat_tests               ;;
21)  batch_grep_count_lines            ;;
22)  batch_bam_subsetting              ;;
23)  batch_backward_compatability      ;;
24)  batch_single_thread               ;; 
25)  batch_copy_ref_section            ;; 
26)  batch_iupac                       ;; 
27)  batch_genols                      ;;
28)  batch_tar_files_from              ;;
29)  batch_gencomp_depn_methods        ;; 
30)  batch_deep                        ;; 
31)  batch_real_world_small_vbs        ;; 
32)  batch_real_world_1_adler32        ;; 
33)  batch_real_world_genounzip_single_process ;; 
34)  batch_real_world_genounzip_compare_file   ;; 
35)  batch_real_world_1_adler32 "--best -f"    ;; 
36)  batch_real_world_1_adler32 "--fast --force-gencomp" ;; 
37)  batch_real_world_optimize         ;;
38)  batch_real_world_with_ref_md5     ;; 
39)  batch_real_world_with_ref_md5 "--best --no-cache --force-gencomp" ;; 
40)  batch_multiseq                    ;;
41)  batch_sam_bam_cram_output         ;;
42)  batch_vcf_bcf_output              ;;
43)  batch_external_unzip              ;;
44)  batch_external_ora                ;;
45)  batch_reference_fastq             ;;
46)  batch_reference_fasta_as_fastq    ;;
47)  batch_reference_sam               ;;
48)  batch_reference_vcf               ;;
49)  batch_many_small_files            ;;
50)  batch_make_reference              ;;
51)  batch_headerless_wrong_ref        ;;
52)  batch_replace                     ;;
53)  batch_coverage_idxstats           ;;
54)  batch_qname_flavors               ;;
55)  batch_piz_no_license              ;;
56)  batch_sendto                      ;;
57)  batch_user_message_permissions    ;;
58)  batch_password_permissions        ;;
59)  batch_reference_backcomp          ;;
60)  batch_real_world_backcomp 11.0.11 ;; # note: versions must match VERSIONS in test/Makefile
61)  batch_real_world_backcomp 12.0.42 ;; 
62)  batch_real_world_backcomp 13.0.21 ;; 
63)  batch_real_world_backcomp 14.0.33 ;; 
64)  batch_real_world_backcomp latest  ;;
65)  batch_basic basic.vcf     latest  ;;
66)  batch_basic basic.bam     latest  ;;
67)  batch_basic basic.sam     latest  ;;
68)  batch_basic basic.fq      latest  ;;
69)  batch_basic basic.fa      latest  ;;
70)  batch_basic basic.bed     latest  ;;
71)  batch_basic basic.gvf     latest  ;;
72)  batch_basic basic.gtf     latest  ;;
73)  batch_basic basic.me23    latest  ;;
74)  batch_basic basic.generic latest  ;;

* ) break; # break out of loop

esac; done

printf "\nALL GOOD! \nstart: $start_date\nend:   `date`\n"

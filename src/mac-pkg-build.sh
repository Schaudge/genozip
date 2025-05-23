#!/bin/bash --norc

# ------------------------------------------------------------------
#   mac-pkg-build.sh
#   Copyright (C) 2020-2025 Genozip Limited where applies
#   Please see terms and conditions in the file LICENSE.txt
#
#   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
#   and subject to penalties specified in the license.

# loosely based on https://github.com/KosalaHerath/macos-installer-builder which is licensed under Apache 2.0 license

if [[ `uname` != 'Darwin' ]]; then
    echo " "
    echo "To create the Mac installer, you must be on a Mac:"
    echo "   1. git pull OR git clone"
    echo "   2. make clean"
    echo "   3. make macos"
    echo " "
    exit 1
fi

MAC_DIR=mac
TARGET_DIR=${MAC_DIR}/target
VERSION=`head -n1 version.h |cut -d\" -f2`
FILES=(genozip genounzip genols genocat) # array
FILES_STR=${FILES[@]} # string

# copy files to target directory
rm -rf $TARGET_DIR || exit 1
mkdir -p ${TARGET_DIR}/darwinpkg/Library/genozip ${TARGET_DIR}/Resources ${TARGET_DIR}/scripts || exit 1

cp ${FILES[@]} ${TARGET_DIR}/darwinpkg/Library/genozip || exit 1
cp ${MAC_DIR}/Distribution ${TARGET_DIR} || exit 1
cp ${MAC_DIR}/uninstall.sh ${TARGET_DIR}/Resources || exit 1
sed -e "s/__VERSION__/${VERSION}/g" ${MAC_DIR}/welcome.html > ${TARGET_DIR}/Resources/welcome.html || exit 1
cp README.md ${TARGET_DIR}/Resources/README.html || exit 1
cp LICENSE.txt ${TARGET_DIR}/Resources/ || exit 1
sed -e "s/__FILES__/${FILES_STR}/g" ${MAC_DIR}/postinstall > ${TARGET_DIR}/scripts/postinstall || exit 1
sed -e "s/__VERSION__/${VERSION}/g" ${MAC_DIR}/uninstall.sh | sed -e "s/__FILES__/${FILES_STR}/g" > ${TARGET_DIR}/darwinpkg/Library/genozip/uninstall.sh || exit 1

chmod -R 755 ${TARGET_DIR} || exit 1

# build package
pkgbuild --identifier org.genozip.${VERSION} --version ${VERSION} --scripts ${TARGET_DIR}/scripts --root ${TARGET_DIR}/darwinpkg ${MAC_DIR}/genozip.pkg || exit 1

PRODUCT=${MAC_DIR}/genozip-installer.pkg
PRODUCT_SIGNED=${MAC_DIR}/genozip-installer.signed.pkg
productbuild --distribution ${TARGET_DIR}/Distribution --resources ${TARGET_DIR}/Resources --package-path ${MAC_DIR} ${PRODUCT} || exit 1

# sign product - note: I need a "3rd party mac developer" certificate installed in the keychain to run this.
# see how to obtain a certificate: https://developer.apple.com/developer-id/
productsign --sign "Divon Lan" ${PRODUCT} ${PRODUCT_SIGNED} || exit 1
pkgutil --check-signature ${PRODUCT_SIGNED}  # doesn't exit with 0 on success
rm ${PRODUCT} || exit 1
mv ${PRODUCT_SIGNED} ${PRODUCT} || exit 1

echo Built ${PRODUCT_SIGNED}
 
exit 0 

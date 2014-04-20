#!/bin/sh
set -x
./autogen.sh
PKG_CONFIG_PATH=$PWD/thirdparty/ck/lib/pkgconfig ./configure --enable-address-sanitizer
make -j
make -j clang-analyze
make check
INST_TEST=/tmp/phenom-install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install

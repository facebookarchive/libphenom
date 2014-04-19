#!/bin/sh
set -x
./autogen.sh
PKG_CONFIG_PATH=$PWD/thirdparty/ck/lib/pkgconfig ./configure --enable-address-sanitizer --with-cares
make -j
make -j clang-analyze
make check

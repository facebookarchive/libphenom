#!/bin/sh
set -x
./autogen.sh
./configure --enable-address-sanitizer --with-cares
make -j
make -j clang-analyze
make check

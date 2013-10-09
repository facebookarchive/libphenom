#!/bin/sh
set -x
./autogen.sh
./configure --enable-address-sanitizer
make -j
make -j clang-analyze
make check

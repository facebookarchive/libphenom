#!/bin/sh
set -x
./autogen.sh
./configure --enable-address-sanitizer
make
make clang-analyze
make check

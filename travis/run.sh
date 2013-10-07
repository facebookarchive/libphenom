#!/bin/sh
set -x
./autogen.sh
./configure
make
make clang-analyze
make check

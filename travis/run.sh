#!/bin/sh
set -x
./autogen.sh
./configure
make
make check

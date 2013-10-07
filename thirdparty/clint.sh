#!/bin/sh
if [ $# -gt 0 ] ; then
  files="$*"
else
  files=$(find corelib include -name \*.c -o -name \*.h)
fi
# Run in parallel
echo $files | xargs -P 8 -n 8 $PYTHON thirdparty/cpplint.py \
  --root=include --filter=-build/include_order --verbose=2 


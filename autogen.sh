#!/bin/sh
# vim:ts=2:sw=2:et:
if test ! -d .acaux ; then
  mkdir .acaux
fi
if test -d "autom4te.cache" ; then
  rm -rf autom4te.cache
fi
aclocal
autoheader
case `uname` in
  Darwin)
    LIBTOOLIZE=${LIBTOOLIZE:-glibtoolize}
    ;;
  *)
    LIBTOOLIZE=${LIBTOOLIZE:-libtoolize}
    ;;
esac
$LIBTOOLIZE --no-warn -i -f
automake --add-missing --foreign
autoconf

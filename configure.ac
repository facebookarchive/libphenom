AC_INIT([phenom], [0.2], [], [phenom])
AC_CONFIG_AUX_DIR([.acaux])
AC_CONFIG_SRCDIR([LICENSE])
AC_CANONICAL_TARGET
AC_LANG(C)

case "$target_os-$target_cpu" in
  solaris*i386)
    dnl force us to consider this a 64-bit system.
    dnl also: you'd have to be insane to run 32-bit intel solaris.
    target_cpu=x86_64
    LDFLAGS="$LDFLAGS -lumem"
    ;;
esac
case "$target_cpu" in
  x86_64|amd64)
    CFLAGS="$CFLAGS -m64 -fno-omit-frame-pointer"
    ;;
	i*86|pentium*|athlon*|k5|k6|k6_2|k6_3)
    target_cpu=i686
    CFLAGS="$CFLAGS -m32 -march=i686 -msse -msse2"
    ;;
esac

dnl We always want large file support in our implementation.
dnl The functions we provide explicitly use 64-bit types and
dnl thus we do not force this requirement on our clients
CFLAGS="$CFLAGS -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"

AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])
LT_INIT

AC_PROG_CC
AC_C_BIGENDIAN
AC_C_INLINE

AM_PROG_CC_C_O
AM_PROG_AS

lenient=no
AC_ARG_ENABLE(lenient, [
  --enable-lenient  Turn off more pedantic levels of warnings
                    and compilation checks
],[
   lenient=$enableval
])

placate_valgrind=no
AC_ARG_ENABLE(placate-valgrind, [
  --enable-placate-valgrind  Attempt to release all memory on shutdown
],[
   placate_valgrind=$enableval
])
if test "$placate_valgrind" == "yes" ; then
  AC_DEFINE(PH_PLACATE_VALGRIND, [1], [Placate valgrind])
fi

stack_protect=no
AC_ARG_ENABLE(stack-protector, [
  --enable-stack-protector  Enable stack protection in the same
                            way that rpmbuild does on some systems.
],[
   stack_protect=$enableval
])

AC_DEFUN([HAVE_COMPILER_FLAG],[
  AC_MSG_CHECKING([if compiler supports $2])
  save_cflags="$CFLAGS"
  CFLAGS="$CFLAGS $2"
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]],[[]])],[
    AC_MSG_RESULT(yes)
    $1=yes
  ],[
    AC_MSG_RESULT(no)
    $1=no
  ])
  CFLAGS="$save_cflags"
])

ASAN=no
AC_ARG_ENABLE(address-sanitizer, [
  --enable-address-sanitizer  Enable ASAN compilation. See
    https://code.google.com/p/address-sanitizer/wiki/AddressSanitizer
],[
   HAVE_COMPILER_FLAG(ASAN, -fsanitize=address)
])

dnl flags we'll export to the pkg-config file
PC_CFLAGS=""
PC_LIBS=""

AC_SEARCH_LIBS([pthread_create], [pthread])
AC_SEARCH_LIBS([socket], [socket])
AC_SEARCH_LIBS([clock_gettime], [rt])
AC_SEARCH_LIBS([inet_pton], [nsl])

PC_LIBS="$LIBS"

case "$CC" in
  *clang*)
    HAVE_CLANG=yes
    ;;
  *)
    HAVE_CLANG=no
    ;;
esac
AM_CONDITIONAL(HAVE_CLANG, test "x$HAVE_CLANG" = xyes)

AC_PATH_PROG(PYTHON, python, true)
AC_PATH_PROG(PKG_CONFIG, pkg-config, false)

if ! $PKG_CONFIG --exists --print-errors openssl ; then
  AC_MSG_ERROR([openssl is required])
fi

pkg_config_libs() {
  pkg_config_rpath=""
  case "$target_os" in
    solaris*|darwin*)
      ;;
    *)
      dnl GNU LD specific; Solaris wants s/L/R/ but moving fast for now
      pkg_config_rpath=`$PKG_CONFIG --libs-only-L $1 | sed -e 's/L/Wl,-rpath=/'`
      ;;
  esac
  echo "`$PKG_CONFIG --libs $1` $pkg_config_rpath"
}

CFLAGS="$CFLAGS `$PKG_CONFIG --cflags openssl`"
LIBS="$LIBS `$PKG_CONFIG --libs openssl`"

case "$target_os" in
  darwin*)
    dnl Apple deprecated the use of their OpenSSL headers in 10.7 and up.
    dnl Pretend we're building for 10.6 and up and the deprecation warnings
    dnl go away
    PC_CFLAGS="$PC_CFLAGS -DMAC_OS_X_VERSION_MIN_REQUIRED=MAC_OS_X_VERSION_10_6"
    ;;
esac

# If you're bumping the CK max-version here, you must also bump it in
# travis/deps.sh otherwise you will break the CI build
if ! $PKG_CONFIG --exists --atleast-version=0.5.0 --max-version=0.5.1 --print-errors ck ; then
  AC_MSG_ERROR([ck is required])
fi
CFLAGS="$CFLAGS `$PKG_CONFIG --cflags ck`"
LIBS="$LIBS `pkg_config_libs ck`"

AC_PATH_PROG(PHP, php, true)
AC_SUBST(PHP)

gimli=no
AC_ARG_WITH(gimli, [
  --with-gimli    Enable support for the gimli process monitor
                  https://bitbucket.org/wez/gimli/
],[
   gimli=$withval
])
if test "x$gimli" != "xno" ; then
  prefixes="$gimli /usr/local /usr"
  mylibdir="lib"
  rpath="-R"
  case $target_os-$target_cpu in
    linux-*64)
      mylibdir="lib64 lib"
      rpath="-Wl,-rpath="
      ;;
    linux*)
      rpath="-Wl,-rpath="
      ;;
  esac
  save_flags="$LDFLAGS"
  save_libs="$LIBS"
  save_cppflags="$CPPFLAGS"
  gimli_dir=""
  AC_MSG_CHECKING([for gimli])
  for dir in $prefixes ; do
    for ldir in $mylibdir ; do
      LDFLAGS="$save_flags $rpath$dir/$ldir -L$dir/$ldir"
      LIBS="$save_libs -lgimli"
      CPPFLAGS="$save_cppflags -I$dir/include"
      AC_TRY_LINK([
        #include <libgimli.h>
      ],[gimli_heartbeat_attach();],
        [gimli_dir=$dir]
      )
      if test -n "$gimli_dir" ; then
        AC_CHECK_HEADERS(libgimli.h)
        save_flags="$save_flags $rpath$dir/$ldir -L$dir/$ldir"
        save_libs="$save_libs -lgimli"
        save_cppflags="$save_cppflags -I$dir/include"
        AC_MSG_RESULT([yes])
        USE_GIMLI=yes
        AC_DEFINE([USE_GIMLI], 1, [Use gimli])
        break
      fi
    done
  done
  LDFLAGS="$save_flags"
  LIBS="$save_libs"
  CPPFLAGS="$save_cppflags"
  if test "xUSE_GIMLI" != xyes ; then
    AC_MSG_RESULT([no])
  fi
fi
AM_CONDITIONAL(GIMLI, test "x$USE_GIMLI" = xyes)

dnl for comparitive benchmarking
AC_ARG_ENABLE(libevent-comparison, [
  --enable-libevent-comparison  Build iopipes.c with libevent support
],[
  SAVE_LIBS="$LIBS"
  AC_SEARCH_LIBS([event_add], [event],[
    HAVE_LIBEVENT=yes
    AC_DEFINE(HAVE_LIBEVENT, 1, have libevent)
  ])
  LIBS="$SAVE_LIBS"
])
AM_CONDITIONAL(HAVE_LIBEVENT, test "x$HAVE_LIBEVENT" = xyes)

have_thread=no
AC_TRY_COMPILE([__thread int tls;],[tls;],
               [have_thread=yes])
if test "$have_thread" == "yes" ; then
  case "$target_os" in
    solaris*)
      dnl I get linker errors with this enabled; seems broken
      ;;
    *)
      AC_DEFINE(HAVE___THREAD, [], [Have __thread TLS])
      ;;
  esac
fi

AC_CHECK_HEADERS(\
alloca.h \
inttypes.h \
locale.h \
port.h \
pthread.h \
stdbool.h \
stddef.h \
stdint.h \
string.h \
sys/inotify.h \
sys/epoll.h \
sys/event.h \
sys/eventfd.h \
sys/param.h \
sys/processor.h \
sys/procset.h \
sys/resource.h \
sys/timerfd.h \
)

dnl needs NBBY on FreeBSD
AC_CHECK_HEADERS([sys/cpuset.h],[],[],[[
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
]])

AC_CHECK_HEADERS([pthread_np.h],[],[],[[
#include <pthread.h>
]])

AC_CHECK_FUNCS(\
accept4 \
asprintf \
backtrace \
backtrace_symbols \
backtrace_symbols_fd \
clock_gettime \
cpuset_setaffinity \
epoll_create \
epoll_create1 \
getpagesize \
inotify_init \
kqueue \
localeconv \
pipe2 \
port_create \
processor_bind \
pthread_getname_np \
pthread_set_name_np \
pthread_setname_np \
pthread_setaffinity_np \
pthread_mach_thread_np \
strerror_r \
strtoll \
sysctlbyname \
thread_policy_set \
timerfd_create \
)

# Do this after we've looked for functions
if test -n "$GCC" ; then
  GCC_VERSION=`$CC -dumpversion`
  GCC_VERSION_MAJOR=`echo $GCC_VERSION | cut -d. -f1`
  GCC_VERSION_MINOR=`echo $GCC_VERSION | cut -d. -f2`

  # Need to select c1x if we want to use _Static_assert
  if expr \( $GCC_VERSION_MAJOR \* 10000 \) \+ \( $GCC_VERSION_MINOR \* 100 \) \>= 40600 >/dev/null ; then
    GCC_STD=gnu1x
  else
    GCC_STD=gnu99
  fi
  # export the std setting so that clients can use ph_static_assert
  PC_CFLAGS="$PC_CFLAGS -std=$GCC_STD"
  CFLAGS="$CFLAGS -std=$GCC_STD \
    -pedantic -Wall -Wextra -Wno-declaration-after-statement \
    -W -Wundef -Wendif-labels -Wshadow -Wpointer-arith -Wcast-align \
    -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes \
    -Wno-nested-externs -Winline -Wdisabled-optimization \
    -fstrict-aliasing -pipe -Wno-parentheses"
fi
AC_SUBST(PC_CFLAGS)
AC_SUBST(PC_LIBS)

if test "$ASAN" == "yes" ; then
  CFLAGS="$CFLAGS -fsanitize=address -fno-common"
else
  if test -n "$GCC" -a "$stack_protect" == "yes" ; then
    CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions \
      -fstack-protector --param=ssp-buffer-size=4"
  fi
fi

dnl CK needs optimization enabled otherwise it breaks.
dnl So turn it on unless an optimization level was explicitly
dnl specified in CFLAGS
case "$CFLAGS" in
  *-O*)
    ;;
  *)
    CFLAGS="$CFLAGS -O2"
    ;;
esac
dnl always include debug info in what we build
CFLAGS="$CFLAGS -g -gdwarf-2"

IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" == "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([include/phenom_build_config.h include/phenom/feature_test.h])
AC_CONFIG_FILES([Makefile libphenom.pc])
AC_OUTPUT

dnl vim:ts=2:sw=2:

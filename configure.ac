AC_INIT([phenom], [1.0], [], [phenom])
AC_CANONICAL_TARGET

case "$target_os-$target_cpu" in
  solaris*i386)
    dnl force us to consider this a 64-bit system.
    dnl also: you'd have to be insane to run 32-bit intel solaris.
    target_cpu=x86_64
    ;;
esac
case "$target_cpu" in
  x86_64|amd64)
    CFLAGS="$CFLAGS -m64"
    ;;
	i*86|pentium*|athlon*|k5|k6|k6_2|k6_3)
    target_cpu=i686
    CFLAGS="$CFLAGS -m32 -march=i686 -msse -msse2"
    ;;
esac

AM_INIT_AUTOMAKE([dist-bzip2 subdir-objects])

AC_PROG_CC
AC_C_BIGENDIAN
AC_C_INLINE
AC_PROG_RANLIB

AM_PROG_CC_C_O
AM_PROG_AS

lenient=no
AC_ARG_ENABLE(lenient, [
  --enable-lenient  Turn off more pedantic levels of warnings
                    and compilation checks
],[
   lenient=$enableval
])

stack_protect=no
AC_ARG_ENABLE(stack-protector, [
  --enable-stack-protector  Enable stack protection in the same
                            way that rpmbuild does on some systems.
],[
   stack_protect=$enableval
])

AC_SEARCH_LIBS([pthread_create], [pthread])
AC_SEARCH_LIBS([socket], [socket])

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
asprintf \
cpuset_setaffinity \
epoll_create \
getpagesize \
inotify_init \
kqueue \
localeconv \
pipe2 \
port_create \
processor_bind \
pthread_set_name_np \
pthread_setname_np \
pthread_setaffinity_np \
pthread_mach_thread_np \
strerror_r \
strtoll \
thread_policy_set \
)

# Virtual memory address size in bits; CK uses this for
# pointer-packing tricks
VMA=unknown
case "$target_cpu" in
  x86_64|amd64)
    MM=CK_MD_TSO
    CPU=X86_64

    case "$target_os" in
      darwin)
        VMA=`sysctl -n machdep.cpu.address_bits.virtual`
        ;;
      linux)
        VMA=`awk '/address sizes/ {print $7;exit}' /proc/cpuinfo`
        ;;
    esac
    ;;
  i686)
    MM=CK_MD_TSO
    CPU=X86
    ;;
  *)
    AC_ERROR([too lazy to port CK for this platform $target_os-$target_cpu])
    ;;
esac
if test "$VMA" == "unknown" ; then
  VMA_BITS="CK_MD_VMA_BITS_UNKNOWN"
  VMA_BITS_VALUE=1
  POINTER_PACK_ENABLE="CK_MD_POINTER_PACK_DISABLE"
else
  VMA_BITS="CK_MD_VMA_BITS"
  VMA_BITS_VALUE="${VMA}ULL"
  POINTER_PACK_ENABLE="CK_MD_POINTER_PACK_ENABLE"
fi
AC_DEFINE_UNQUOTED(PHENOM_TARGET_CPU, PHENOM_TARGET_CPU_$CPU,
                   "Target platform name")
AC_SUBST(MM)
AC_SUBST(VMA_BITS)
AC_SUBST(VMA_BITS_VALUE)
AC_SUBST(POINTER_PACK_ENABLE)

# Do this after we've looked for functions
if test -n "$GCC" ; then
  CFLAGS="$CFLAGS -std=gnu99 \
    -pedantic -Wall -Wextra -Wno-declaration-after-statement \
    -W -Wundef -Wendif-labels -Wshadow -Wpointer-arith -Wcast-align \
    -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes \
    -Wnested-externs -Winline -Wdisabled-optimization \
    -fstrict-aliasing -pipe -Wno-parentheses"
fi

if test -n "$GCC" -a "$stack_protect" == "yes" ; then
  CFLAGS="$CFLAGS -Wp,-D_FORTIFY_SOURCE=2 -fexceptions \
    -fstack-protector --param=ssp-buffer-size=4"
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
CFLAGS="$CFLAGS -g"

IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" == "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([include/phenom/config.h])
AC_CONFIG_FILES([Makefile thirdparty/ck/include/ck_md.h])
AC_OUTPUT

dnl vim:ts=2:sw=2:


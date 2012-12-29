AC_INIT([phenom], [1.0], [], [phenom])
AC_CANONICAL_TARGET
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

have_thread=no
AC_TRY_COMPILE([__thread int tls;],[tls;],
               [have_thread=yes])
if test "$have_thread" == "yes" ; then
  AC_DEFINE(HAVE___THREAD, [], [Have __thread TLS])
fi

AC_CHECK_HEADERS(\
alloca.h \
inttypes.h \
locale.h \
pthread.h \
pthread_np.h \
stdbool.h \
stddef.h \
stdint.h \
string.h \
sys/cpuset.h \
sys/inotify.h \
sys/epoll.h \
sys/event.h \
sys/eventfd.h \
sys/resource.h \
sys/timerfd.h \
)
AC_CHECK_FUNCS(\
asprintf \
cpuset_setaffinity \
epoll_create \
getpagesize \
inotify_init \
kqueue \
localeconv \
pipe2 \
pthread_set_name_np \
pthread_setname_np \
pthread_setaffinity_np \
pthread_mach_thread_np \
strerror_r \
strtoll \
thread_policy_set \
)

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
      *)
        VMA=unknown
        ;;
    esac
    ;;
  *)
    AC_ERROR([too lazy to port CK for this platform])
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
  CFLAGS="$CFLAGS -D_XOPEN_SOURCE=600 -D_BSD_SOURCE \
    -D_POSIX_C_SOURCE=200809 -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
    -std=gnu99 \
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
IRONMANCFLAGS=""
if test -n "$GCC" -a "$lenient" == "no" ; then
  IRONMANCFLAGS="-Werror"
fi
AC_SUBST(IRONMANCFLAGS)

AC_CONFIG_HEADER([include/phenom/config.h])
AC_CONFIG_FILES([Makefile thirdparty/ck/include/ck_md.h])
AC_OUTPUT

dnl vim:ts=2:sw=2:


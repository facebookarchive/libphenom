/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PHENOM_DEFS_H
#define PHENOM_DEFS_H

/**
 * # Base Environment
 *
 * Including `phenom/defs.h` sets the base environment for using
 * phenom.  This header should be included first (most phenom headers
 * will pull this in explicitly) so that the compilation environment
 * exposes the more modern unix compilation features of your system.
 */

#define PHENOM_TARGET_CPU_X86_64 1
#define PHENOM_TARGET_CPU_X86    2

#ifndef _REENTRANT
# define _REENTRANT
#endif
#define __EXTENSIONS__ 1
#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#define _POSIX_C_SOURCE 200809
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <phenom/config.h>

/* This is working around eccentricities in the CK build system */
#if PHENOM_TARGET_CPU == PHENOM_TARGET_CPU_X86_64
# ifndef __x86_64__
#  define __x86_64__ 1
# endif
#elif PHENOM_TARGET_CPU == PHENOM_TARGET_CPU_X86
# ifndef __x86__
#  define __x86__ 1
# endif
#else
# error unsupported target platform
#endif

#ifdef __FreeBSD__
/* need this to get u_short so we can include sys/event.h.
 * This has to happen before we include sys/types.h */
# include <sys/cdefs.h>
# define __BSD_VISIBLE 1
#endif

#include <sys/types.h>

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#endif
#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif
#ifdef HAVE_STDBOOL_H
# include <stdbool.h>
#endif
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#ifdef HAVE_SYS_TIMERFD_H
# include <sys/timerfd.h>
#endif
#ifdef HAVE_SYS_EVENTFD_H
# include <sys/eventfd.h>
#endif
#ifdef HAVE_SYS_EPOLL_H
# include <sys/epoll.h>
#endif
#ifdef HAVE_SYS_EVENT_H
# include <sys/event.h>
#endif

#ifdef HAVE_PTHREAD_H
# include <pthread.h>
#endif
#ifdef HAVE_PTHREAD_NP_H
# include <pthread_np.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>

#ifdef HAVE_SYS_CPUSET_H
# include <sys/cpuset.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_SYS_PROCESSOR_H
# include <sys/processor.h>
#endif
#ifdef HAVE_SYS_PROCSET_H
# include <sys/procset.h>
#endif

#ifdef HAVE_LOCALE_H
# include <locale.h>
#endif

#ifdef HAVE_PORT_CREATE
# include <port.h>
# include <sys/poll.h>
#endif

#include <sys/uio.h>

#if defined(__APPLE__)
/* for thread affinity */
#include <mach/thread_policy.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
/* clock */
#include <mach/mach_time.h>
#endif

/**
 * ## Pedantic compilation
 *
 * To stave off undefined or unexpected conditions, Phenom is
 * compiled in an extremely unforgiving mode that causes warnings
 * to be treated as errors.
 *
 * There are a couple of useful source annotations that you can
 * use to avoid triggering some classes of error.
 *
 * ### unused_parameter
 *
 * ```
 * void myfunc(int not_used)
 * {
 *    unused_parameter(not_used);
 * }
 * ```
 *
 * ### Result not ignored
 *
 * Some compilation environments are very strict and will raise
 * warnings if you ignore return values of certain functions.
 * In some cases you really do want to ignore these results.
 * Here's how to tell the compiler to leave you alone:
 *
 * ```
 * void myfunc(void)
 * {
 *    ignore_result(poll(&pfd, 1, 100));
 * }
 * ```
 */

#if defined(PHENOM_IMPL)
// Use this to eliminate 'unused parameter' warnings
# define unused_parameter(x)  (void)x

// Use this to cleanly indicate that we intend to ignore
// the result of functions marked with warn_unused_result
# if defined(__USE_FORTIFY_LEVEL) && __USE_FORTIFY_LEVEL > 0
#  define ignore_result(x) \
  do { __typeof__(x) _res = x; (void)_res; } while(0)
# else
#  define ignore_result(x) x
# endif

# ifdef __GNUC__
#  define PH_GCC_VERSION (__GNUC__ * 10000 \
    + __GNUC_MINOR__ * 100 \
    + __GNUC_PATCHLEVEL__)
# else
#  define PH_GCC_VERSION 0
#endif

# ifdef __GNUC__
#  define likely(x)    __builtin_expect(!!(x), 1)
#  define unlikely(x)  __builtin_expect(!!(x), 0)
# else
#  define likely(x)    (x)
#  define unlikely(x)  (x)
# endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Generic result type
 *
 * If you wish to avoid TLS overheads with errno if/when a function fails,
 * you may choose to implement your return values in terms of the ph_result_t
 * type.  The value `PH_OK` is defined to 0 and means success.  All other
 * values are interpreted as something not quite working out for a variety
 * of reasons.
 *
 * * `PH_OK` - success!
 * * `PH_NOMEM` - insufficient memory
 * * `PH_BUSY` - too busy to complete now (try later)
 * * `PH_ERR` - generic failure (sorry)
 * * `PH_NOENT` - requested item has no entry, could not be found
 * * `PH_EXISTS` - requested item is already present
 * * `PH_DONE` - operation (iteration) completed
 */
typedef uint32_t ph_result_t;
#define PH_OK      0
#define PH_NOMEM   1
#define PH_BUSY    2
#define PH_ERR     3 /* programmer too lazy */
#define PH_NOENT   4
#define PH_EXISTS  5
#define PH_DONE    6

# ifdef __GNUC__
#  define ph_offsetof(type, field) __builtin_offsetof(type, field)
# else
#  define ph_offsetof(type, field) ((size_t)(&((type *)0)->field))
# endif
#define ph_container_of(ptr_, type_, member_)  \
    ((type_ *)((char *)ptr_ - ph_offsetof(type_, member_)))

#define ph_static_assert_paste2(pre, post)  pre ## post
#define ph_static_assert_paste1(pre, post)  ph_static_assert_paste2(pre, post)

#if 0 /* fake prototype for documentation purposes */
/** Perform a compile time assertion
 *
 * To perform compile time assertion checking (useful for things like ABI
 * checks), you may use the ph_static_assert() macro.  Usage is as follows:
 *
 * ```
 * ph_static_assert(sizeof(int)==4, assuming_32_bits);
 * ```
 *
 * The first parameter is the constant expression to check.  It has to be
 * constant for the compiler to check it at compile time.  If you wish to
 * assert that function parameters are correct, you should use ph_assert()
 * instead.
 *
 * The second parameter is ideally a meaningful identifier string that gives a
 * descriptive label for the issue.  It has to be a valid identifier component
 * in order to provide meaningful error messages on a wider range of compilers.
 *
 * If the assertion fails, you'll encounter an error message like this:
 *
 * ```
 * error: zero width for bit-field ‘static_assertion_failed_assuming_32_bits’
 * ```
 *
 * If you are using GCC 4.6 or later you'll see an error message like this:
 *
 * ```
 * error: static assertion failed: "assuming_32_bits"
 * ```
 */
void ph_static_assert(bool constexpr, identifier_message);
#endif

# if PH_GCC_VERSION >= 40600
#  define ph_static_assert(expr, msg)   _Static_assert(expr, #msg)
# elif PH_GCC_VERSION >= 40300
   /* has the __COUNTER__ construct */
#  define ph_static_assert(expr, msg) \
     typedef struct { \
       int ph_static_assert_paste1(static_assertion_failed_,msg) : \
       !!(expr); \
     } ph_static_assert_paste1(static_assertion_failed_,__COUNTER__)
# else
   /* this can generate conflicting type names if
    * the same assert message is used from the same line of two
    * different files */
#  define ph_static_assert(expr, msg) \
     typedef struct { \
       int ph_static_assert_paste1(static_assertion_failed_,msg) : \
       !!(expr); \
     } ph_static_assert_paste1(ph_static_assert_paste1(\
            static_assertion_failed_,msg),__LINE__)
# endif

/** Log a PH_LOG_PANIC level message, then abort()
 *
 * This logs a PANIC level message using ph_log(), logs the current
 * stacktrace using ph_log_stacktrace(), and then calls `abort()`.
 *
 * It is intended to be used in situations where the world must
 * have an immediate end.
 * */
void ph_panic(const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 1, 2)))
  __attribute__((noreturn))
#endif
  ;

#if 0 /* fake prototype for documentation purposes */
/** Perform a runtime assertion
 *
 * To perform a runtime assertion, use ph_assert() as shown below.
 * Phenom makes the assumption that the expression to be asserted
 * will almost never be true.
 *
 * ```
 * ph_assert(n < 10, "n is within range");
 * ```
 *
 * If your compiler is GCC 4.3 or newer, we'll try to evaluate the
 * expression at compile time to save you some surprises.  This
 * works similarly to ph_static_assert().  You'll see an error
 * message along the lines of:
 *
 * ```
 * file.c:350: error: call to ‘failed_assert_1’ declared with attribute error: 17 < 16 :: nargs too big
 * ```
 *
 * Otherwise, the error will only trigger when the code is executed.
 *
 * Unlike assert(), ph_assert() is always enabled and compiled into
 * your code.
 *
 * If you'd like assertions that are disabled when `NDEBUG` is `#define`d,
 * you may use ph_debug_assert() instead.
 */
void ph_assert(bool condition, const char *message);

/** Perform a runtime assertion in debug code
 *
 * Works exactly like ph_assert(), except that the assertion code is
 * elided from a "production" build; one that has `NDEBUG` defined.
 *
 * ```
 * // Never checked in the production build, because there are
 * // never going to be bugs there...
 * ph_debug_assert(n < 10, "n is within range");
 * ```
 */
void ph_debug_assert(bool condition, const char *message);
#endif

# if PH_GCC_VERSION >= 40300
// You'd think that you could use _Static_assert in here, if your
// compiler supported it, but it ends up not seeing the const
// folded expression and complains about it not being a constant
// expression.  If you try to use _Static_assert(0, msg) then
// gcc will generate spurious asserts.  So we're stuck with this
// implementation, which is pretty decent at the end of the day
# define ph_assert_static_inner(expr, msg, id) do { \
    extern void ph_static_assert_paste1(failed_assert_,id)(void)\
      __attribute__((error(#expr " :: " msg))); \
    ph_static_assert_paste1(failed_assert_,id)(); \
} while (0)
# define ph_assert(expr, msg) do { \
  if (__builtin_constant_p(expr) && !(expr)) { \
    ph_assert_static_inner(expr, msg, __COUNTER__); \
  } \
  if (unlikely(expr)) { \
    ph_panic("assertion " #expr " failed: " msg); \
  } \
} while(0)
#else
# define ph_assert(expr, msg) do { \
  if (unlikely(expr)) { \
    ph_panic(msg); \
  } \
} while(0)

#endif

#ifdef NDEBUG
# define ph_debug_assert(expr, msg) ((void)0)
#else
# define ph_debug_assert(expr, msg) ph_assert(expr, msg)
#endif

/** Holds a socket descriptor */
typedef int ph_socket_t;

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


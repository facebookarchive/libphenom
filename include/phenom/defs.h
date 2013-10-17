/*
 * Copyright 2012-present Facebook, Inc.
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

// Ensure that PRIu32 and friends get defined for both C99
// and C++ consumers of libphenom
#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS
#endif

#ifdef __FreeBSD__
/* need this to get u_short so we can include sys/event.h.
 * This has to happen before we include sys/types.h */
# include <sys/cdefs.h>
# define __BSD_VISIBLE 1
#endif

#include <sys/types.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/uio.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

# ifdef PHENOM_IMPL
#  include "phenom_build_config.h"

# ifdef HAVE_ALLOCA_H
#  include <alloca.h>
# endif
# ifdef HAVE_SYS_TIMERFD_H
#  include <sys/timerfd.h>
# endif
# ifdef HAVE_SYS_EVENTFD_H
#  include <sys/eventfd.h>
# endif
# ifdef HAVE_SYS_EPOLL_H
#  include <sys/epoll.h>
# endif
# ifdef HAVE_SYS_EVENT_H
#  include <sys/event.h>
# endif

# ifdef HAVE_PTHREAD_NP_H
#  include <pthread_np.h>
# endif
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <unistd.h>
# include <errno.h>
# include <signal.h>
# include <fcntl.h>
# include <limits.h>

# ifdef HAVE_SYS_CPUSET_H
#  include <sys/cpuset.h>
# endif
# ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
# endif
# ifdef HAVE_SYS_PROCESSOR_H
#  include <sys/processor.h>
# endif
# ifdef HAVE_SYS_PROCSET_H
#  include <sys/procset.h>
# endif

# ifdef HAVE_LOCALE_H
#  include <locale.h>
# endif

# ifdef HAVE_PORT_CREATE
#  include <port.h>
#  include <sys/poll.h>
# endif

# if defined(__APPLE__)
  /* for thread affinity */
# include <mach/thread_policy.h>
# include <mach/mach_init.h>
# include <mach/thread_act.h>
  /* clock */
# include <mach/mach_time.h>
# endif
# endif

# ifdef __GNUC__
#  define PH_GCC_VERSION (__GNUC__ * 10000 \
    + __GNUC_MINOR__ * 100 \
    + __GNUC_PATCHLEVEL__)
# else
#  define PH_GCC_VERSION 0
#endif

// Helpers for pasting __LINE__ for symbol generation
#define ph_defs_paste2(pre, post)  pre ## post
#define ph_defs_paste1(pre, post)  ph_defs_paste2(pre, post)
#if PH_GCC_VERSION >= 40300
# define ph_defs_gen_symbol(pre)    ph_defs_paste1(pre, __COUNTER__)
#else
# define ph_defs_gen_symbol(pre)    ph_defs_paste1(pre, __LINE__)
#endif

#ifdef __cplusplus
extern "C" {
#endif


#if 0 /* fake prototype for documentation purposes */
/** Records an initialization and finalization routine with a specific priority
 *
 * This macro sets up a constructor routine to record the init
 * entry.  These init entries are processed when ph_library_init()
 * is called by the process.  You should restrict initialization to
 * low level actions like preparing memtypes, initializing locks
 * and bootstrapping subsystems.  You **must not** make these functions
 * dependent upon configuration, as the configuration will not have
 * been loaded at the time these are invoked.
 *
 * This exists to facilitate initialization of the library itself,
 * but you may also use it aid with registering modules and facilities
 * with the library.  For example, the `PH_TYPE_FORMATTER_REGISTER`
 * uses this mechanism to arrange for ph_vprintf_register() to be
 * called at the appropriate time.
 *
 * You may specify either `initfn` or `finifn` as NULL; this just means
 * that the item doesn't have an initializer or finalizer, respectively.
 * It probably doesn't make much sense to specify both as NULL, but it is
 * allowed and does not cause an error.
 *
 * ```
 * static void init(void) {
 *   mt = ph_memtype_register(...);
 * }
 * PH_LIBRARY_INIT(init, 0)
 * ```
 */
void PH_LIBRARY_INIT_PRI(initfn, finifn, pri);

/** Records an initialization and finalization routine
 *
 * Delegates to PH_LIBRARY_INIT_PRI() with a default priority.
 */
void PH_LIBRARY_INIT(initfn, finifn);
#endif

/** Records an initialization and finalization routine.
 *
 * Use PH_LIBRARY_INIT_PRI() or PH_LIBRARY_INIT() rather than using
 * this directly.
 *
 * The relative priorities of entries are computed thus:
 *
 * * The lowest `pri` values are initialized first.
 * * If two items have the same `pri` value, the `file` values are
 *   ordered using using strcmp.
 * * If two items have the same `file`, the item with the lowest
 *   `line` value is initialized first.
 * * If the `line` values are the same, the address of the entry is
 *   used to order the items relative to each other.
 *
 * At finalization time, the reverse of the above ordering is used;
 * the items will be finalized in the opposite of the order that they
 * were initialized.
 */
struct ph_library_init_entry {
  // the file where this init entry is defined
  const char *file;
  // the line number where this init entry is defined
  uint32_t line;
  // the priority of this init entry.
  // Clients of the library should use values >= 100.
  // During init, the `init` functions are calling in order of ascending
  // priority.  During finalization, the `fini` functions are called in
  // order of descending priority.
  int pri;
  // initializer (may be NULL)
  void (*init)(void);
  // finalizer (may be NULL)
  void (*fini)(void);
  // this is really a ck_stack_entry_t but we don't want to pull in
  // that definition from here.  This is protected by a static assert
  // in corelib/init.c
  intptr_t st;
};

void ph_library_init_register(struct ph_library_init_entry *ent);
#define PH_LIBRARY_INIT_PRI(initfn, finifn, pri) \
  static __attribute__((constructor)) \
  void ph_defs_gen_symbol(ph__lib__init__)(void) { \
    static struct ph_library_init_entry ent = { \
      __FILE__, __LINE__, pri, initfn, finifn, 0 \
    }; \
    ph_library_init_register(&ent); \
  }
#define PH_LIBRARY_INIT(initfn, finifn) \
  PH_LIBRARY_INIT_PRI(initfn, finifn, 100)

/**
 * ## Pedantic compilation
 *
 * To stave off undefined or unexpected conditions, libPhenom is
 * compiled in an extremely unforgiving mode that causes warnings
 * to be treated as errors.
 *
 * There are a couple of useful source annotations that you can
 * use to avoid triggering some classes of error.
 *
 * ### ph_unused_parameter
 *
 * ```
 * void myfunc(int not_used)
 * {
 *    ph_unused_parameter(not_used);
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
 *    ph_ignore_result(poll(&pfd, 1, 100));
 * }
 * ```
 */

// Use this to eliminate 'unused parameter' warnings
# define ph_unused_parameter(x)  (void)x

// Use this to cleanly indicate that we intend to ignore
// the result of functions marked with warn_unused_result
# if defined(__USE_FORTIFY_LEVEL) && __USE_FORTIFY_LEVEL > 0
#  define ph_ignore_result(x) \
  do { __typeof__(x) _res = x; (void)_res; } while (0)
# else
#  define ph_ignore_result(x) x
# endif

# ifdef __GNUC__
#  define ph_likely(x)    __builtin_expect(!!(x), 1)
#  define ph_unlikely(x)  __builtin_expect(!!(x), 0)
# else
#  define ph_likely(x)    (x)
#  define ph_unlikely(x)  (x)
# endif

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
#  define ph_offsetof(type, field) \
      ((size_t)(&((type *)0)->field)) // NOLINT(runtime/casting)
# endif
#define ph_container_of(ptr_, type_, member_)  \
    ((type_ *)(void*)((char *)ptr_ - ph_offsetof(type_, member_)))

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
 * error: zero width for bit-field 'static_assertion_failed_assuming_32_bits'
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
# else
   /* this can generate conflicting type names if
    * the same assert message is used from the same line of two
    * different files on some compilers */
#  define ph_static_assert(expr, msg) \
     typedef struct { \
       int ph_defs_paste1(static_assertion_failed_, msg) : \
       !!(expr); \
     } ph_defs_gen_symbol(static_assertion_failed_)
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
 * libPhenom makes the assumption that the expression to be asserted
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
 * file.c:350: error: call to 'failed_assert_1' declared with attribute error: 17 < 16 :: nargs too big
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
    extern void ph_defs_paste1(failed_assert_, id)(void)\
      __attribute__((error(#expr " :: " msg))); \
    ph_defs_paste1(failed_assert_, id)(); \
} while (0)
# define ph_assert(expr, msg) do { \
  if (__builtin_constant_p(expr) && !(expr)) { \
    ph_assert_static_inner(expr, msg, __COUNTER__); \
  } \
  if (ph_unlikely(!(expr))) { \
    ph_panic("assertion " #expr " failed: " msg " at %s:%d", \
        __FILE__, __LINE__); \
  } \
} while (0)
#else
# define ph_assert(expr, msg) do { \
  if (ph_unlikely(!(expr))) { \
    ph_panic("assertion " #expr " failed: " msg " at %s:%d", \
        __FILE__, __LINE__); \
  } \
} while (0)

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


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

#if defined(__APPLE__)
/* for thread affinity */
#include <mach/thread_policy.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#endif

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

typedef int64_t phenom_time_t;

typedef uint32_t phenom_result_t;
#define PHENOM_OK      0
#define PHENOM_NOMEM   1
#define PHENOM_BUSY    2
#define PHENOM_ERR     3 /* programmer too lazy */


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


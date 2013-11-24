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

#ifndef PHENOM_SYSUTIL_H
#define PHENOM_SYSUTIL_H

#include "phenom/defs.h"
#include "phenom/socket.h"
#include "phenom/memory.h"

/**
 * # Utility Functions
 *
 * A slightly random set of helper functions.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
# define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define PH_PIPE_NONBLOCK 1
#define PH_PIPE_CLOEXEC  2
/** Create a pipe and optional set flags.
 *
 * The flags parameter can be one or more of:
 *
 * * `PH_PIPE_NONBLOCK` - set non-blocking IO
 * * `PH_PIPE_CLOEXEC` - set the CLOEXEC flag
 *   or'd together.
 */
ph_result_t ph_pipe(ph_socket_t fds[2], int flags);

struct ph_pingfd {
  ph_socket_t fds[2];
};

typedef struct ph_pingfd ph_pingfd_t;

ph_result_t ph_pingfd_init(ph_pingfd_t *pfd);
ph_result_t ph_pingfd_ping(ph_pingfd_t *pfd);
ph_result_t ph_pingfd_close(ph_pingfd_t *pfd);
ph_socket_t ph_pingfd_get_fd(ph_pingfd_t *pfd);
bool ph_pingfd_consume_one(ph_pingfd_t *pfd);
uint32_t ph_pingfd_consume_all(ph_pingfd_t *pfd);

void ph_freedtoa(char *s);
char *ph_dtoa(double _d, int mode, int ndigits,
    int *decpt, int *sign, char **rve);
double ph_strtod(const char *s00, const char **se);

/** Initialize the library
 *
 * Must be called prior to calling into any other phenom
 * library functions.
 *
 * When invoked for the first time, invokes the `init` routines
 * that have been registered using the PH_LIBRARY_INIT() and
 * PH_LIBRARY_INIT_PRI() macros.  The init routines are invoked
 * in order of ascending priority.
 *
 * Arranges for the `fini` routines to be called in order of descending
 * priority when the process terminates, using atexit().
 *
 * ph_library_init() is safe to be called concurrently from multiple
 * threads, and safe to invoke multiple times from the same thread.
 *
 * You may use ph_library_init() to bootstrap the thread-local variables
 * needed for libphenom to operate.  See ph_thread_self().
 */
ph_result_t ph_library_init(void);

struct timeval ph_time_now(void);

/** round up to next power of 2 */
static inline uint32_t ph_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

/** Return the number of physical cores in the system */
uint32_t ph_num_cores(void);

/** Generate a unique temporary file name and open it.
 * nametemplate must be of the form `/path/to/fileXXXXXX`.  The
 * 'X' characters will be replaced by randomized characters.
 * flags is passed to the underlying open(2) call.
 *
 * Returns the opened file descriptor, or -1 on error
 */
int ph_mkostemp(char *nametemplate, int flags);

/** Generate a unique temporary name with a suffix and open it.
 * nametemplate must not be NULL; it must be of the form
 * `/path/to/fileXXXXXsuffix.`  The 'X' characters will be replaced
 * by randomized characters.  suffixlen identifies the length of
 * the filename suffix.
 *
 * flags is passed to the underlying open(2) call.
 *
 * Returns the opened file descriptor, or -1 on error
 */
int ph_mkostemps(char *nametemplate, int suffixlen, int flags);

/** Thread-safe errno value to string conversion */
const char *ph_strerror(int errval);
/** Thread-safe errno value to string conversion */
const char *ph_strerror_r(int errval, char *buf, size_t len);

/** 128-bit murmur hash implementation; result goes into out (which
 * can just be a uint64_t[2]). */
void ph_hash_bytes_murmur(const void *key, const int len,
    const uint32_t seed, void *out);

void ph_debug_console_start(const char *unix_sock_path);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


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

typedef int ph_socket_t;

/** Set or disable non-blocking mode for a file descriptor */
void ph_socket_set_nonblock(ph_socket_t fd, bool enable);

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

void ph_freedtoa(char *s);
char *ph_dtoa(double _d, int mode, int ndigits,
    int *decpt, int *sign, char **rve);
double ph_strtod(const char *s00, const char **se);

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

struct ph_vprintf_funcs {
  bool (*print)(void *arg, const char *buf, size_t len);
  bool (*flush)(void *arg);
};

/** Thread-safe errno value to string conversion */
const char *ph_strerror(int errval);
/** Thread-safe errno value to string conversion */
const char *ph_strerror_r(int errval, char *buf, size_t len);

/** Portable string formatting.
 * This handles things like NULL string pointers without faulting.
 * It does not support long doubles nor does it support hex double
 * formatting.  It does not support locale for decimal points;
 * we always print those as `.`
 *
 * Extensions: some helpers are provided to format information
 * from the phenom library.  These extensions are designed such
 * that the gcc printf format type checking can still be used;
 * we prefix the extended format specifier with a backtick and
 * a 'P' character.
 *
 * For instance, \`Pe%d is seen as `%d` by GCC's checker
 * but the entire \`Pe%d is replaced by the strerror expansion.
 *
 * ```none
 *  `Pe%d -   replaced by the return from strerror(arg), using
 *            strerror_r() when present, where arg is an errno
 *            argument supplied by you.
 *  `Pv%s%p - recursively expands a format string and a va_list.
 *            Arguments are a char* and ph_vaptr(va_list)
 * ```
 *
 */
int ph_vprintf_core(void *print_arg,
    const struct ph_vprintf_funcs *print_funcs,
    const char *fmt0, va_list ap);

#ifdef __sun__
# define ph_vaptr(ap)    (void*)ap
#else
# define ph_vaptr(ap)    (void*)ap
#endif

/** Like vsnprintf(), except that it uses ph_vprintf_core() */
int ph_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
/** Like snprintf(), except that it uses ph_vprintf_core() */
int ph_snprintf(char *buf, size_t size, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;

/** Uses ph_vprintf_core to print to a file descriptor.
 * Uses a 1k buffer internally to reduce the number of calls
 * to the write() syscall. */
int ph_vfdprintf(int fd, const char *fmt, va_list ap);
/** Uses ph_vprintf_core to print to a file descriptor */
int ph_fdprintf(int fd, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Like asprintf, except that it uses ph_vprintf_core().
 * On error, returns -1 and sets strp to NULL */
int ph_vasprintf(char **strp, const char *fmt, va_list ap);
int ph_asprintf(char **strp, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Like ph_asprintf, except that it uses the specified
 * memtype for the memory it allocates.
 * On error, returns -1 and sets strp to NULL */
int ph_vmtsprintf(ph_memtype_t memtype, char **strp,
    const char *fmt, va_list ap);
int ph_mtsprintf(ph_memtype_t memtype, char **strp,
    const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;

/** 128-bit murmur hash implementation; result goes into out (which
 * can just be a uint64_t[2]). */
void ph_hash_bytes_murmur(const void *key, const int len,
    const uint32_t seed, void *out);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


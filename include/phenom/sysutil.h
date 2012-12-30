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

#ifdef __cplusplus
extern "C" {
#endif

typedef int phenom_socket_t;
void phenom_socket_set_nonblock(phenom_socket_t fd, bool enable);

#define PH_PIPE_NONBLOCK 1
#define PH_PIPE_CLOEXEC  2
phenom_result_t phenom_pipe(phenom_socket_t fds[2], int flags);


struct phenom_pingfd {
  phenom_socket_t fds[2];
};

typedef struct phenom_pingfd phenom_pingfd_t;

phenom_result_t phenom_pingfd_init(phenom_pingfd_t *pfd);
phenom_result_t phenom_pingfd_ping(phenom_pingfd_t *pfd);
phenom_result_t phenom_pingfd_close(phenom_pingfd_t *pfd);
phenom_socket_t phenom_pingfd_get_fd(phenom_pingfd_t *pfd);
bool phenom_pingfd_consume_one(phenom_pingfd_t *pfd);

void phenom_freedtoa(char *s);
char *phenom_dtoa(double _d, int mode, int ndigits,
    int *decpt, int *sign, char **rve);
double phenom_strtod(const char *s00, const char **se);

static inline uint32_t phenom_power_2(uint32_t n)
{
  n |= (n >> 16);
  n |= (n >> 8);
  n |= (n >> 4);
  n |= (n >> 2);
  n |= (n >> 1);
  return n + 1;
}

struct phenom_vprintf_funcs {
  bool (*print)(void *arg, const char *buf, size_t len);
  bool (*flush)(void *arg);
};

/** Thread-safe errno value to string conversion */
const char *phenom_strerror(int errval);
const char *phenom_strerror_r(int errval, char *buf, size_t len);

/** Portable string formatting.
 * This handles things like NULL string pointers without faulting.
 * It does not support long doubles nor does it support hex double
 * formatting.  It does not support locale for decimal points;
 * we always print those as ".".
 *
 * Extensions: some helpers are provided to format information
 * from the phenom library.  These extensions are designed such
 * that the gcc printf format type checking can still be used;
 * we prefix the extended format specifier with a backtick and
 * a 'P' character.
 *
 * For instance, "`Pe%d" is seen as "%d" by GCC's checker
 * but the entire "`Pe%d" is replaced by the strerror expansion.
 *
 *  `Pe%d -   replaced by the return from strerror(arg), using
 *            strerror_r() when present, where arg is an errno
 *            argument supplied by you.
 *  `Pv%s%p - recursively expands a format string and a va_list.
 *            Arguments are a char* and va_list*
 */
int phenom_vprintf_core(void *print_arg,
    const struct phenom_vprintf_funcs *print_funcs,
    const char *fmt0, va_list ap);

/** Like vsnprintf(), except that it uses phenom_vprintf_core() */
int phenom_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
/** Like snprintf(), except that it uses phenom_vprintf_core() */
int phenom_snprintf(char *buf, size_t size, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;

/** Uses phenom_vprintf_core to print to a file descriptor.
 * Uses a 1k buffer internally to reduce the number of calls
 * to the write() syscall. */
int phenom_vfdprintf(int fd, const char *fmt, va_list ap);
/** Uses phenom_vprintf_core to print to a file descriptor */
int phenom_fdprintf(int fd, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Like asprintf, except that it uses phenom_vprintf_core().
 * On error, returns -1 and sets strp to NULL */
int phenom_vasprintf(char **strp, const char *fmt, va_list ap);
int phenom_asprintf(char **strp, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Like phenom_asprintf, except that it uses the specified
 * memtype for the memory it allocates.
 * On error, returns -1 and sets strp to NULL */
int phenom_vmtsprintf(phenom_memtype_t memtype, char **strp,
    const char *fmt, va_list ap);
int phenom_mtsprintf(phenom_memtype_t memtype, char **strp,
    const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


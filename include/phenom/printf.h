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

#ifndef PHENOM_PRINTF_H
#define PHENOM_PRINTF_H

#include "phenom/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_vprintf_funcs {
  bool (*print)(void *arg, const char *buf, size_t len);
  bool (*flush)(void *arg);
};

/** Defines a named formatter
 * `formatter_arg` is the `arg` that was registered by
 * ph_vprintf_register().
 * `object` is the value identified by the %p in the format string.
 * `print_arg` should be passed as the first arg to `funcs->print`.
 * `funcs` is used to carry out the printing
 */
typedef size_t (*ph_vprintf_named_formatter_func)(
    void *formatter_arg,
    void *object,
    void *print_arg,
    const struct ph_vprintf_funcs *funcs);

/** Register a named formatter
 *
 * The suggested usage is:
 *
 * ```
 * PH_TYPE_FORMATTER_FUNC(myfunc) {
 *   // available locals are per ph_vprintf_named_formatter_func
 *   funcs->print(funcs->print_arg, "myfunc", strlen("myfunc"));
 *   return strlen("myfunc");
 * }
 * ```
 *
 * This will automatically register your formatting function during
 * ph_library_init().
 */
bool ph_vprintf_register(const char *name, void *formatter_arg,
    ph_vprintf_named_formatter_func func);

#define PH_TYPE_FORMATTER_REGISTER(tname) \
static void ph_vprintf_named_formatter_register_##tname(void) { \
  ph_vprintf_register(#tname, NULL, \
      ph_vprintf_named_formatter_func_##tname); \
} \
PH_LIBRARY_INIT(ph_vprintf_named_formatter_register_##tname, 0)

#define PH_TYPE_FORMATTER_FUNC(tname) \
size_t ph_vprintf_named_formatter_func_##tname(void*, void*, void*, \
    const struct ph_vprintf_funcs*); \
PH_TYPE_FORMATTER_REGISTER(tname) \
size_t ph_vprintf_named_formatter_func_##tname(\
    CK_CC_UNUSED void *formatter_arg, void *object, void *print_arg, \
    const struct ph_vprintf_funcs *funcs)

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
 *  `Ps%p -   replaced by the contents of the ph_string_t* argument
 *  `Ps%d%p - replaced by the first n bytes specified by the integer
 *            argument of the ph_string_t* argument.
 *  `P{name:%p} - Looks up "name" in the formatting callback table.
 *                Passes the object specified by the pointer argument
 *                to the formatted and is replaced by the result
 * ```
 *
 */
int ph_vprintf_core(void *print_arg,
    const struct ph_vprintf_funcs *print_funcs,
    const char *fmt0, va_list ap);

#define ph_vaptr(ap)    (void*)&ap

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



#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


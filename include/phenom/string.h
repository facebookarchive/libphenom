/*
 * Copyright 2013 Facebook, Inc.
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

#ifndef PHENOM_STRING_H
#define PHENOM_STRING_H

/**
 * # Strings
 *
 * C strings are somewhat bare, so we provide some helpers that aim to
 * assist with:
 *
 * * avoiding heap allocations where possible
 * * tracking heap allocations when not avoidable
 * * safely manage string expansion and growth
 *
 * # Stack-based strings
 *
 * To avoid touching the allocator in hot code paths, it is desirable
 * to construct and operate on strings local to the stack.  There are
 * two special kinds of string that can aid you in this case.
 *
 * ## Statically allocated fixed size buffers
 *
 * If you know that you will never exceed a maximum length for your string
 * data in a given function, you can declare a stack based string buffer
 * like this:
 *
 * ```
 * void myfunc(void) {
 *    PH_STRING_DECLARE_STACK(mystr, 128);
 *
 *    ph_string_printf(&mystr, "Hello %s", "world");
 * }
 * ```
 *
 * The above creates a string that can hold up to 128 bytes of text.
 * Attempting to print beyond the end of it will be silently ignored.
 *
 * Since the buffer resides on the stack, it doesn't require any
 * cleanup.
 *
 * ## Statically allocated growable buffers
 *
 * If you know that the common case for your function is that the data
 * will typically fit within a reasonably small threshold but you also
 * need to be able to deal with occasional values that exceed it,
 * you can declare a stack based string buffer that will be promoted
 * to a heap allocated string if the size overflows:
 *
 * ```
 * void myfunc(void) {
 *    ph_memtype_t my_memtype; // you need to arrange for this to be valid
 *    PH_STRING_DECLARE_GROW(mystr, 128, my_memtype);
 *
 *    ph_string_printf(&mystr, "Hello %s", "world");
 *
 *    ph_string_delref(&mystr);
 * }
 * ```
 *
 * Since the string may end up referencing heap allocated memory, we
 * *MUST* call `ph_string_delref` before the stack unwinds so that we
 * can avoid a memory leak.
 *
 * # Embedding in structs
 *
 * It is sometimes desirable to embed an instance inside another structure.
 * To facilitate this, you may use the following pattern:
 *
 * ```
 * struct mystruct {
 *   char mybuf[128];
 *   ph_string_t mystr;
 * };
 *
 * struct mystruct *myfunc(void) {
 *   struct mystruct *s = calloc(1, sizeof(*s));
 *
 *   ph_string_init_claim(&s->mystr, PH_STRING_STATIC,
 *       &s->mybuf, 0, sizeof(s->mybuf));
 *
 *   return s;
 * }
 *
 * void myfreefunc(struct mystruct *s) {
 *    ph_string_delref(&s->mystr);
 *    free(s);
 * }
 * ```
 *
 * You may substitute `PH_STRING_STATIC` with `PH_STRING_GROW_MT(mymemtype)`
 * to configure the string to switch to using heap allocated buffers using
 * your specified memtype if they overflow the static size.
 *
 * # Referencing in printf functions
 *
 * ```none
 *  `Ps%p -   replaced by the contents of the ph_string_t* argument
 *  `Ps%d%p - replaced by the first n bytes specified by the integer
 *            argument of the ph_string_t* argument.
 * ```
 *
 */

#include "phenom/refcnt.h"
#include "phenom/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_string;
typedef struct ph_string ph_string_t;

struct ph_string {
  ph_refcnt_t ref;
  ph_memtype_t mt;
  uint32_t len, alloc;
  char *buf;
  ph_string_t *slice;
  bool onstack;
};

#define PH_STRING_STATIC       PH_MEMTYPE_INVALID
#define PH_STRING_GROW_MT(mt)  -(mt)

#define PH_STRING_DECLARE_GROW(name, size, mt) \
  char _str_buf_grow_##name[size]; \
  ph_string_t name = { 1, PH_STRING_GROW_MT(mt), 0, size, \
    _str_buf_grow_##name, 0, true }

#define PH_STRING_DECLARE_STACK(name, size) \
  char _str_buf_static_##name[size]; \
  ph_string_t name = { 1, PH_STRING_STATIC, 0, size, \
    _str_buf_static_##name, 0, true }


/** Initialize a string from a static or unmanaged buffer
 *
 * Similar to `ph_string_make_claim` except that the string
 * is either a stack based string or is embedded in some other
 * storage.
 *
 * When the final reference on such a string is released,
 * only the buffers that it points to will be released, not
 * the string object itself.
 */
void ph_string_init_claim(ph_string_t *str,
    ph_memtype_t mt, char *buf, uint32_t len, uint32_t size);

/** Make a new string instance from a source buffer
 *
 * Allocates a new string object that claims ownership
 * of the provided buffer.
 *
 * The returned string object has a single reference;
 * call ph_string_delref() on it to destroy it.  At that
 * time, the buffer it claimed will be freed against the
 * the specified memtype.
 *
 * If the memtype is PH_STRING_STATIC, the buffer will
 * be referenced but not freed or grown by the string functions.
 */
ph_string_t *ph_string_make_claim(ph_memtype_t mt,
    char *buf, uint32_t len, uint32_t size);

/** Make a new string instance by copying a buffer
 *
 * A convenience function that is logically equivalent to allocating
 * a buffer of `size` bytes, copying `len` bytes from `buf` into it,
 * and then claiming that buffer via ph_string_make_claim
 */
ph_string_t *ph_string_make_copy(ph_memtype_t mt,
    const char *buf, uint32_t len, uint32_t size);

/** Make a new string from a C-String
 */
ph_string_t *ph_string_make_cstr(ph_memtype_t mt, const char *str);

/** Make a new empty string instance
 *
 * The string will be configured to grow on demand, using
 * the specified memtype.
 *
 * The string will allocate `size` buffer to initialize the
 * string.
 */
ph_string_t *ph_string_make_empty(ph_memtype_t mt,
    uint32_t size);

/** Add a reference to a string
 */
static inline void ph_string_addref(ph_string_t *str)
{
  ph_refcnt_add(&str->ref);
}

/** Release a reference to a string
 *
 * When the final reference is released, the string is
 * destroyed and its resources released.
 */
void ph_string_delref(ph_string_t *str);

/** Append a memory buffer to a ph_string_t instance
 */
ph_result_t ph_string_append_buf(ph_string_t *str,
    const char *buf, uint32_t len);

/** Append a C-string to a ph_string_t instance
 */
ph_result_t ph_string_append_cstr(
    ph_string_t *str, const char *cstr);

/** Returns the length of the string contents, in bytes
 */
static inline uint32_t ph_string_len(ph_string_t *str) {
  return str->len;
}

/** Compare the value of two strings for equality
 *
 * Return true if the strings are equal based on a raw binary
 * comparison of the string buffers.  This function ignores locale
 * and other nuances of multi-byte encodings.
 */
bool ph_string_equal(ph_string_t *a, ph_string_t *b);

/** Compare the value of the string against a C-string for equality
 */
bool ph_string_equal_cstr(ph_string_t *a, const char *b);

/** Compare the value of two strings
 *
 * Compares two strings, returning an integer value less than
 * zero if `a` is considered less than `b`, 0 if `a` == `b` or
 * greater than zero if `a` is considered greater than `b`.
 *
 * This function ignores locale and other nuances of multi-byte encodings.
 */
int ph_string_compare(ph_string_t *a, ph_string_t *b);

/** Formatted print to string
 */
int ph_string_vprintf(ph_string_t *a, const char *fmt, va_list ap);

/** Formatted print to string
 *
 * Uses ph_vprintf_core()
 */
int ph_string_printf(ph_string_t *a, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Make a string from a formatted string
 */
ph_string_t *ph_string_make_printf(ph_memtype_t mt, uint32_t size,
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


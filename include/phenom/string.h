/*
 * Copyright 2013-present Facebook, Inc.
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
 * # Static constant strings
 *
 * Sometimes you'll want to define a string to represent a constant c-string
 * value in your code, for instance, as a fall back for a hash table key.
 * You may use `PH_STRING_DECLARE_STATIC` as a shortcut for this case; it
 * can only be used with a string literal parameter; this is to avoid calling
 * strlen at runtime:
 *
 * ```
 * void myfunc(void) {
 *   PH_STRING_DECLARE_STATIC(mystr, "Hello world");
 * }
 * ```
 *
 * ```COUNTEREXAMPLE
 * void myfunc(const char *str) {
 *   PH_STRING_DECLARE_STATIC(mystr, str); // BAD: will get the wrong size
 * }
 * ```
 *
 * If you want/need to use it with a variable (not a string literal), then you
 * can and should use `PH_STRING_DECLARE_STATIC_CSTR` instead:
 *
 * ```
 * void myfunc(const char *str) {
 *   PH_STRING_DECLARE_STATIC_CSTR(mystr, str);
 * }
 * ```
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

#define PH_STRING_DECLARE_STATIC(name, cstr) \
  ph_string_t name = { 1, PH_STRING_STATIC, sizeof(cstr)-1, \
    sizeof(cstr), (char*)cstr, 0, true }

#define PH_STRING_DECLARE_STATIC_CSTR_INNER(name, cstr, len) \
  uint32_t len = strlen(cstr); \
  ph_string_t name = { 1, PH_STRING_STATIC, len, \
    len + 1, (char*)cstr, 0, true }
#define PH_STRING_DECLARE_STATIC_CSTR(name, cstr) \
  PH_STRING_DECLARE_STATIC_CSTR_INNER(name, cstr, ph_defs_gen_symbol(len))

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

/** Initialize a string as a slice over another string.
 *
 * Maintains a reference to the sliced string that is released
 * when this one is released.
 */
void ph_string_init_slice(ph_string_t *str,
    ph_string_t *slice, uint32_t start, uint32_t len);

/** Make a new string instance by slicing over another string.
 *
 * Maintains a reference to the sliced string that is released
 * when this one is released.
 */
ph_string_t *ph_string_make_slice(ph_string_t *str,
    uint32_t start, uint32_t len);

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

/** Append a string to a string
 *
 * The contents of `str` are appended to `target`
 */
static inline ph_result_t ph_string_append_str(ph_string_t *target,
    ph_string_t *str)
{
  return ph_string_append_buf(target, str->buf, str->len);
}

/** Append a C-string to a ph_string_t instance
 */
ph_result_t ph_string_append_cstr(
    ph_string_t *str, const char *cstr);

/** Append a series of UTF-16 code points to a string and encode as UTF-8
 *
 * * `codepoints` points to a buffer of code points
 * * `numpoints` specifies the number of code points
 * * `bytes` if not NULL, receives the number of UTF-8 bytes that were
 *   appended to the string
 */
ph_result_t ph_string_append_utf16_as_utf8(
    ph_string_t *str, int32_t *codepoints, uint32_t numpoints,
    uint32_t *bytes);

/** Iterate a sequence of UTF-8 characters, returning UTF-16 codepoints
 *
 * This function implements an iterator that knows how to decode UTF-8
 * encoded multibyte text into UTF-16 code points.
 *
 * To begin iterating, set `offset` to the starting offset of the UTF-8
 * text.  This offset is measured in bytes from the start of the data
 * in the string.
 *
 * UTF-8 bytes will be decoded from that position and a UTF-16 codepoint
 * will be stored into `*codepoint`.  `*offset` will be updated so that
 * it holds the offset of the next UTF-8 byte.
 *
 * If a UTF-16 byte was successfully decoded, returns `PH_OK`.
 * If a partial or invalid UTF-8 sequence was detected, returns `PH_ERR`.
 * If the end of the string was reached, returns `PH_DONE`.
 *
 * ```
 * uint32_t off = 0;
 * int32_t cp;
 *
 * while (ph_string_iterate_utf8_as_utf16(str, &off, &cp) == PH_OK) {
 *   // got a codepoint in cp
 * }
 * ```
 */
ph_result_t ph_string_iterate_utf8_as_utf16(
    ph_string_t *str, uint32_t *offset, int32_t *codepoint);

/** Returns true if the string is valid UTF-8
 *
 * Checks the content of the string buffer, returns true if
 * we can iterate the length of the string and produce a valid
 * series of UTF-16 codepoints from it, or false otherwise.
 */
bool ph_string_is_valid_utf8(ph_string_t *str);

static inline uint8_t ph_utf8_seq_len(uint8_t first) {
  if (first < 0x80) {
    return 1;
  }
  if (first <= 0xc1) {
    return 0;
  }
  if (first <= 0xdf) {
    return 2;
  }
  if (first <= 0xef) {
    return 3;
  }
  if (first <= 0xf4) {
    return 4;
  }
  return 0;
}

/** Returns the length of the string contents, in bytes
 */
static inline uint32_t ph_string_len(const ph_string_t *str) {
  return str->len;
}

/** Resets the string length to zero
 *
 * Sets the length to zero, effectively clearly the string
 * contents and rewinding the append position to the start
 * of the string.
 *
 * This is useful in cases where you desire to reuse the string
 * buffer and avoid additional heap allocations.
 */
static inline void ph_string_reset(ph_string_t *str) {
  str->len = 0;
}

/** Compare the value of two strings for equality
 *
 * Return true if the strings are equal based on a raw binary
 * comparison of the string buffers.  This function ignores locale
 * and other nuances of multi-byte encodings.
 */
bool ph_string_equal(const ph_string_t *a, const ph_string_t *b);

/** Compare the value of two strings for equality, case insensitive
 *
 * Return true the strings compare equal, with each byte passed
 * to the tolower() function.  Aside form lower casing each byte,
 * this function ignores locale and other nuances of multi-byte
 * encodings.
 */
bool ph_string_equal_caseless(const ph_string_t *a, const ph_string_t *b);

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
int ph_string_compare(const ph_string_t *a, const ph_string_t *b);

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

#if 0 // Dummy for docs
/** Copy a NUL-terminated C-string from a ph_string to a stack buffer
 *
 * Since ph_string_t does not maintain C-string NUL termination,
 * and many library functions require a C-string representation,
 * there will be times where you need to get one.
 *
 * This macro declares a local variable named `localvarname` using
 * the C99 variable sized array feature to create a stack buffer,
 * and copies the string data to the stack buffer and null terminates
 * the buffer.
 *
 * Usage:
 *
 * ```
 * void open_a_file(ph_string_t *name) {
 *   PH_STRING_DECLARE_AND_COPY_CSTR(filename, name);
 *   int fd = open(filename, O_RDONLY);
 * }
 * ```
 */
void PH_STRING_DECLARE_AND_COPY_CSTR(localvarname, ph_string_t *str);

/** Obtain a NUL-terminated C-string pointer from a ph_string
 *
 * This macro declares a local `const char *` variable named
 * `localvarname` and initializes it to reference a C-string representation
 * of the provided `str` parameter.
 *
 * If `str` happens to be NUL terminated, or can be safely NUL terminated,
 * `localvarname` is set to point to the buffer in `str` and no copying is
 * performed.
 *
 * If `str` cannot be NUL terminated, uses the C99 variable sized array
 * feature to create storage on the stack, copies the string data to
 * the stack, NUL-terminates it and sets `localvarname` to reference it.
 *
 * `localvarname` is a const variable because it may reference the internal
 * string buffer.  `localvarname` should be considered to be invalidated
 * by any changes made to the `str`.
 *
 * Usage:
 *
 * ```
 * void open_a_file(ph_string_t *name) {
 *   PH_STRING_DECLARE_CSTR_AVOID_COPY(filename, name);
 *   int fd = open(filename, O_RDONLY);
 * }
 * ```
 *
 * This is invalid usage because the string is modified:
 *
 * ```COUNTEREXAMPLE
 * void do_something(ph_string_t *name) {
 *   PH_STRING_DECLARE_CSTR_AVOID_COPY(namecstr, name);
 *   ph_string_append_cstr(name, "woot");
 *   // WRONG! because the append above means that namecstr was invalidated
 *   // and is no longer safe to use
 *   ph_fd_printf(STDOUT_FILENO, "namecstr is %s", namecstr);
 * }
 * ```
 */
void PH_STRING_DECLARE_CSTR_AVOID_COPY(localvarname, ph_string_t *str);
#endif

#define PH_STRING_DECLARE_AND_COPY_CSTR(name, str) \
  char name[ph_string_len(str)+1]; \
  memcpy(name, (str)->buf, ph_string_len(str)); \
  name[ph_string_len(str)] = '\0'

// Check for, or arrange to, NUL terminate a string ready for grabbing
// a c-string representation
bool _ph_string_nul_terminated(ph_string_t *str);

#define PH_STRING_DECLARE_CSTR_AVOID_COPY_INNER(name, namebuf, str) \
  bool ph_defs_paste1(namebuf, _terminated) = _ph_string_nul_terminated(str); \
  char namebuf[ph_defs_paste1(namebuf, _terminated) ? 1 : str->len + 1]; \
  const char *name = namebuf; \
  if (ph_defs_paste1(namebuf, _terminated)) { \
    name = str->buf; \
  } else { \
    memcpy(namebuf, str->buf, str->len); \
    namebuf[str->len] = '\0'; \
  }

#define PH_STRING_DECLARE_CSTR_AVOID_COPY(name, str) \
  PH_STRING_DECLARE_CSTR_AVOID_COPY_INNER(name, \
      ph_defs_gen_symbol(name##_avoid_copy_), (str))

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


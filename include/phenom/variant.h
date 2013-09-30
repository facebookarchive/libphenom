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
/* Portions of the code and the bulk of the pack/unpack documentation are:
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

/**
 * # Variant Data Type
 *
 * Variants can represent runtime variable values and are primarily
 * useful when it comes to serialization to/from the JSON or BSER encoding
 * formats.
 *
 * Note: JSON requires that all strings be UTF-8 encoded, but these
 * functions won't validate the strings you pass into them because there
 * are situations where binary string support is desirable or essential.
 * You may use ph_string_is_valid_utf8() to check for correctness.
 *
 * ## Building Values
 *
 * This section describes functions that help to create, or pack, complex
 * values, especially nested objects and arrays. Value building is based on a
 * format string that is used to tell the functions about the expected
 * arguments.  The builder API is derived from equivalent functionality in
 * the excellent [Jansson](http://www.digip.org/jansson/) library.
 *
 * For example, the format string `i` specifies a single integer value, while
 * the format string `[ssb]` or the equivalent `[s, s, b]` specifies an array
 * value with two strings and a boolean as its items:
 *
 * ```
 * // Create the integer 42
 * ph_variant_t *v = ph_var_pack("i", 42);
 *
 * // Create the array ["foo", "bar", true]
 * v = ph_var_pack("[ssb]", "foo", "bar", true);
 * ```
 *
 * Here's the full list of format characters. The type in parentheses denotes
 * the resulting variant type, and the type in brackets (if any) denotes the
 * C type that is expected as the corresponding argument.
 *
 * `z` (string) [const char \*]
 * Convert a NULL (`z`ero) terminated C-string to a variant string.
 * The string is copied and a fresh ph_string_t is constructed.
 *
 * `s` (string) [ph_string_t \*]
 * Convert a ph_string_t reference to a variant string.  Does not add a
 * reference to the provided string; the reference is stolen by the container.
 *
 * `S` (string) [ph_string_t \*]
 * Convert a ph_string_t reference to a variant string.  Adds a reference
 * to the provided string.
 *
 * `n` (null)
 * Output a variant null value. No argument is consumed.
 *
 * `b` (boolean) [int]
 * Convert a C int to variant boolean value. Zero is converted to false and
 * non-zero to true.
 *
 * `i` (integer) [int]
 * Convert a C int to variant integer.
 *
 * `I` (integer) [int64_t]
 * Convert a C int64_t to variant integer.
 *
 * `f` (real) [double]
 * Convert a C double to variant real.
 *
 * `o` (any value) [ph_variant_t \*]
 * Output any given variant value as-is. If the value is added to an array or
 * object, the reference to the value passed to o is stolen by the container.
 *
 * `O` (any value) [ph_variant_t \*]
 * Like o, but the argument's reference count is incremented. This is useful
 * if you pack into an array or object and want to keep the reference for the
 * variant value consumed by `O` to yourself.
 *
 * `[fmt]` (array)
 * Build an array with contents from the inner format string. fmt may contain
 * objects and arrays, i.e. recursive value building is supported.
 *
 * `{fmt}` (object)
 * Build an object with contents from the inner format string fmt. The first,
 * third, etc. format character represent a key, and must be `s` (as object keys
 * are always strings). The second, fourth, etc. format character represent a
 * value. Any value may be an object or array, i.e. recursive value building
 * is supported.
 *
 * Whitespace, `:` and `,` are ignored; you may use them to improve the
 * readability of your format strings.
 *
 * More examples:
 *
 * ```
 * // Build an empty variant object
 * ph_var_pack(NULL, "{}");
 *
 * // Build the variant object {"foo": 42, "bar": 7}
 * ph_var_pack(NULL, "{sisi}", "foo", 42, "bar", 7);
 *
 * // Like above, ':', ',' and whitespace are ignored
 * ph_var_pack(NULL, "{s:i, s:i}", "foo", 42, "bar", 7);
 *
 * // Build the variant array [[1, 2], {"cool": true}]
 * ph_var_pack(NULL, "[[i,i],{s:b}]", 1, 2, "cool", true);
 * ```
 *
 * ## Parsing and Validating Values
 *
 * This section describes functions that help to validate complex values and
 * extract, or unpack, data from them. Like building values, this is also
 * based on format strings.
 *
 * While a variant value is unpacked, the type specified in the format string
 * is checked to match that of the variant value.  This is the validation part
 * of the process. In addition to this, the unpacking functions can also check
 * that all items of arrays and objects are unpacked. This check be enabled
 * with the format character `!` or by using the flag `PH_VAR_STRICT`.
 * See below for details.
 *
 * Here's the full list of format characters. The type in parentheses denotes
 * the variant type, and the type in brackets (if any) denotes the C type
 * whose address should be passed.
 *
 * `s` (string) [ph_string_t \*]
 * Store the string pointer into the ph_string_t parameter.  The reference
 * count is not incremented.
 *
 * `S` (string) [ph_string_t \*]
 * Store the string pointer into the ph_string_t parameter.  The reference
 * is incremented.
 *
 * `n` (null)
 * Expect a variant null value. Nothing is extracted.
 *
 * `b` (boolean) [bool]
 * Convert a variant boolean value to a C bool.
 *
 * `i` (integer) [int]
 * Convert a variant integer to C int.
 *
 * `I` (integer) [int64_t]
 * Convert a variant integer to C int64_t.
 *
 * `f` (real) [double]
 * Convert a variant real to C double.
 *
 * `F` (integer or real) [double]
 * Convert a variant number (integer or real) to C double.
 *
 * `o` (any value) [ph_variant_t \*]
 * Store a variant value with no conversion to a ph_variant_t pointer.
 *
 * `O` (any value) [ph_variant_t \*]
 * Like `O`, but the variant value's reference count is incremented.
 *
 * `[fmt]` (array)
 * Convert each item in the variant array according to the inner format string.
 * fmt may contain objects and arrays, i.e. recursive value extraction is
 * supported.
 *
 * `{fmt}` (object)
 * Convert each item in the variant object according to the inner format
 * string fmt. The first, third, etc. format character represent a key, and
 * must be `s`. The corresponding argument to unpack functions is read as the
 * object key. The second fourth, etc. format character represent a value and
 * is written to the address given as the corresponding argument. Note that
 * every other argument is read from and every other is written to.
 *
 * fmt may contain objects and arrays as values, i.e. recursive value
 * extraction is supporetd.
 *
 * Any `s` representing a key may be suffixed with a `?` to make the key
 * optional. If the key is not found, nothing is extracted. See below for
 * an example.
 *
 * `!`
 * This special format character is used to enable the check that all object
 * and array items are accessed, on a per-value basis. It must appear inside
 * an array or object as the last format character before the closing bracket
 * or brace. To enable the check globally, use the `PH_VAR_STRICT` unpacking
 * flag.
 *
 * `*`
 * This special format character is the opposite of `!`. If the `PH_VAR_STRICT`
 * flag is used, `*` can be used to disable the strict check on a per-value
 * basis. It must appear inside an array or object as the last format
 * character before the closing bracket or brace.
 *
 * Whitespace, `:` and `,` are ignored; you may use them to improve the
 * readability of your format strings.
 *
 * The following unpacking flags are available:
 *
 * * `PH_VAR_STRICT` - Enable the extra validation step checking that all
 *   object and array items are unpacked. This is equivalent to appending the
 *   format character `!` to the end of every array and object in the format
 *   string.
 * * `PH_VAR_VALIDATE_ONLY` - Don't extract any data, just validate the value
 *   against the given format string. Note that object keys must still be
 *   specified after the format string.
 *
 * Examples:
```
// root is the variant integer 42
int myint;
ph_var_unpack(root, &err, 0, "i", &myint);
assert(myint == 42);

// root is the variant object {"foo": "bar", "quux": true}
ph_string_t *str;
int boolean;
ph_var_unpack(root, &err, 0, "{s:s, s:b}", "foo", &str, "quux", &boolean);
assert(strcmp(str, "bar") == 0 && boolean == 1);

// root is the variant array [[1, 2], {"baz": null}
ph_var_unpack(root, &err, PH_VAR_VALIDATE_ONLY, "[[i,i], {s:n}]", "baz");
// returns PH_OK for validation success, nothing is extracted

// root is the variant array [1, 2, 3, 4, 5]
int myint1, myint2;
ph_var_unpack(root, &err, 0, "[ii!]", &myint1, &myint2);
// returns -1 for failed validation

// root is an empty variant object
int myint = 0, myint2 = 0;
ph_var_unpack(root, &err, 0, "{s?i, s?[ii]}",
            "foo", &myint1,
            "bar", &myint2, &myint3);
// myint1, myint2 or myint3 is no touched as "foo" and "bar" don't exist
```
 *
 * # JSONPath style queries
 *
 * Simple [JSONPath](http://goessner.net/articles/JsonPath/) style queries may be
 * used to interrogate variants.  libPhenom supports only a limited subset of
 * JSONPath.
 *
 * The query must be started at the root (using the `$` character) and will
 * return only a single value; wildcard selection is not supported.
 *
 * * `$` selects the root object
 * * `.` focuses on a child of the current cursor
 * * `name` selects an object with the specified name
 * * `[1]` selects the 2nd array element from the current cursor (arrays are zero
 *    based).
 *
 * For example, given the object:
 *
```json
{
  "one": {
    "two": ["a", "b", "c", {
      "lemon": "cake"
    }]
  }
}
```
 *
 * The query `$.one.two[2]` produces the value `"c"`, while the
 * query `$.one.two[3].lemon` produces the value `"cake"`.
 *
 * Use ph_var_jsonpath_get() to issue JSONPath style queries.
 */

#ifndef PHENOM_VARIANT_H
#define PHENOM_VARIANT_H

#include "phenom/defs.h"
#include "phenom/refcnt.h"
#include "phenom/memory.h"
#include "phenom/queue.h"
#include "phenom/hashtable.h"
#include "phenom/string.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PH_VAR_OBJECT,
  PH_VAR_ARRAY,
  PH_VAR_STRING,
  PH_VAR_INTEGER,
  PH_VAR_REAL,
  PH_VAR_TRUE,
  PH_VAR_FALSE,
  PH_VAR_NULL
} ph_variant_type_t;

struct ph_variant {
  ph_refcnt_t ref;
  ph_variant_type_t type;
  union {
    int64_t ival;
    double  dval;
    ph_string_t *sval;
    struct {
      uint32_t len, alloc;
      struct ph_variant **arr;
    } aval;
    ph_ht_t oval;
  } u;
};

/** Represents a runtime variable data type */
typedef struct ph_variant ph_variant_t;

#define PH_VAR_ERROR_TEXT_LENGTH    160
struct ph_var_err {
  uint32_t line, column, position;
  // Transient failures are likely due to memory limitations or any class
  // of issue that means that a later attempt to parse this same data might
  // result in a successful outcome.
  bool transient;
  char text[PH_VAR_ERROR_TEXT_LENGTH];
};
typedef struct ph_var_err ph_var_err_t;


/** Returns the type of a variant */
static inline ph_variant_type_t ph_var_type(const ph_variant_t *var) {
  return var->type;
}

/** Construct a boolean variant value */
ph_variant_t *ph_var_bool(bool val);

/** Returns the boolean value of a boolean variant
 *
 * Returns false if the variant is not actually a boolean.
 */
static inline bool ph_var_bool_val(ph_variant_t *var) {
  if (var->type == PH_VAR_TRUE) {
    return true;
  }
  return false;
}

/** Returns true if the variant is a boolean */
static inline bool ph_var_is_boolean(ph_variant_t *var) {
  return var->type == PH_VAR_TRUE || var->type == PH_VAR_FALSE;
}

/** Construct a null variant value */
ph_variant_t *ph_var_null(void);

/** Returns true if the variant is a NULL value */
static inline bool ph_var_is_null(ph_variant_t *var) {
  return var->type == PH_VAR_NULL;
}

/** Release a variant value */
void ph_var_delref(ph_variant_t *var);

/** Add a reference to a variant value */
static inline void ph_var_addref(ph_variant_t *var) {
  ph_refcnt_add(&var->ref);
}

/** Construct an integer variant value */
ph_variant_t *ph_var_int(int64_t ival);

/** Returns the integer value of a variant
 *
 * Returns 0 if the value is not an integer
 */
static inline int64_t ph_var_int_val(const ph_variant_t *var) {
  switch (var->type) {
    case PH_VAR_INTEGER:
      return var->u.ival;
    default:
      return 0;
  }
}

/** Returns true if the variant holds an integer value */
static inline bool ph_var_is_int(ph_variant_t *var) {
  return var->type == PH_VAR_INTEGER;
}

/** Construct a real variant value */
ph_variant_t *ph_var_double(double dval);

/** Returns the double value of a variant
 *
 * Returns 0.0 if the value is not a double
 */
static inline double ph_var_double_val(const ph_variant_t *var) {
  switch (var->type) {
    case PH_VAR_REAL:
      return var->u.dval;
    default:
      return 0.0;
  }
}

/** Returns true if the variant holds a double value */
static inline bool ph_var_is_double(ph_variant_t *var) {
  return var->type == PH_VAR_REAL;
}

/** Construct and claim a string variant value
 *
 * This steals a reference from str.
 */
ph_variant_t *ph_var_string_claim(ph_string_t *str);

/** Construct a string variant value
 *
 * Adds a reference to str and wraps it in a variant.
 */
ph_variant_t *ph_var_string_make(ph_string_t *str);

/** Construct a variant from a C-string
 */
ph_variant_t *ph_var_string_make_cstr(const char *cstr);

/** Returns the string value of a variant
 *
 * Returns NULL if the variant does not hold a string.
 *
 * Does **not** add a reference to the string value.
 */
static inline ph_string_t *ph_var_string_val(const ph_variant_t *var) {
  switch (var->type) {
    case PH_VAR_STRING:
      return var->u.sval;
    default:
      return 0;
  }
}

/** Returns true if a variant contains a string value */
static inline bool ph_var_is_string(ph_variant_t *var) {
  return var->type == PH_VAR_STRING;
}

/** Construct a variant array
 *
 * It is pre-sized to hold the specified number of elements, but
 * is initially empty.
 */
ph_variant_t *ph_var_array(uint32_t nelems);

/** Returns the number of elements in the variant array.
 *
 * Returns 0 if the variant is not an array.
 */
static inline uint32_t ph_var_array_size(ph_variant_t *var) {
  switch (var->type) {
    case PH_VAR_ARRAY:
      return var->u.aval.len;
    default:
      return 0;
  }
}

/** Returns true if the variant is an array */
static inline bool ph_var_is_array(ph_variant_t *var) {
  return var->type == PH_VAR_ARRAY;
}

/** Append a variant to an array
 *
 * The array may need to be grown to accommodate the new value,
 * in which case the append operation can fail and this function
 * will return `PH_NOMEM`.
 *
 * If successful, adds a reference to the added value and returns `PH_OK`.
 */
ph_result_t ph_var_array_append(ph_variant_t *arr, ph_variant_t *val);

/** Append a variant to an array and claim its reference
 *
 * The array may need to be grown to accommodate the new value,
 * in which case the append operation can fail and this function
 * will return `PH_NOMEM`.
 *
 * If successful, returns `PH_OK`.
 */
ph_result_t ph_var_array_append_claim(ph_variant_t *arr, ph_variant_t *val);

/** Get array element at a given position
 *
 * Returns the array element, borrowing its reference (addref is not
 * called).
 */
static inline ph_variant_t *ph_var_array_get(ph_variant_t *arr, uint32_t pos)
{
  switch (arr->type) {
    case PH_VAR_ARRAY:
      if (pos >= arr->u.aval.len) {
        return NULL;
      }
      return arr->u.aval.arr[pos];
    default:
      return NULL;
  }
}

/** Set array element at a given position to val, and claim its reference.
 *
 * Replaces the value at the given position.  You may not use this
 * function to create a hole in the array. Valid positions are 0 through
 * the length of the array.
 */
ph_result_t ph_var_array_set_claim(ph_variant_t *arr, uint32_t pos,
    ph_variant_t *val);

/** Set array element at a given position to val.
 *
 * Replaces the value at the given position.  You may not use this
 * function to create a hole in the array. Valid positions are 0 through
 * the length of the array.
 *
 * If successful, adds a reference to the value.
 */
ph_result_t ph_var_array_set(ph_variant_t *arr, uint32_t pos,
    ph_variant_t *val);

/** Construct a variant object
 *
 * It is pre-sized to hold the specified number of elements, but is
 * initially empty.
 */
ph_variant_t *ph_var_object(uint32_t nelems);

/** Returns true if the variant is an object */
static inline bool ph_var_is_object(ph_variant_t *var) {
  return var->type == PH_VAR_OBJECT;
}

/** Returns the number of key/value pairs in an object
 */
uint32_t ph_var_object_size(ph_variant_t *var);

/** Set or replace a key/value pair, claiming the key and value
 */
ph_result_t ph_var_object_set_claim_kv(ph_variant_t *obj,
    ph_string_t *key, ph_variant_t *val);

/** Set or replace a key/value pair, adding a ref to key and value.
 */
ph_result_t ph_var_object_set(ph_variant_t *obj,
    ph_string_t *key, ph_variant_t *val);

/** Set or replace a key/value pair, claiming the value
 *
 * This variation allows setting the key using a C-string
 */
ph_result_t ph_var_object_set_claim_cstr(ph_variant_t *obj,
    const char *cstr, ph_variant_t *val);

/** Delete a key/value pair
 */
ph_result_t ph_var_object_del(ph_variant_t *obj, ph_string_t *key);

/** Get the value for a given key
 *
 * Returns NULL if the key isn't present in the object, otherwise
 * returns a borrowed reference to the value.
 */
ph_variant_t *ph_var_object_get(ph_variant_t *obj, ph_string_t *key);

/** Get the value for a given C-string key
 *
 * Returns NULL if the key isn't present in the object, otherwise
 * returns a borrowed reference to the value.
 */
ph_variant_t *ph_var_object_get_cstr(ph_variant_t *obj, const char *key);

/** Begin iterating an object value
 *
 * Delegates to ph_ht_iter_first()
 */
bool ph_var_object_iter_first(ph_variant_t *obj, ph_ht_iter_t *iter,
    ph_string_t **key, ph_variant_t **val);

/** Continue iterating an object value
 */
bool ph_var_object_iter_next(ph_variant_t *obj, ph_ht_iter_t *iter,
    ph_string_t **key, ph_variant_t **val);

/** Begin iterating an object value in key order
 *
 * Delegates to ph_ht_ordered_iter_first().
 * You must call ph_var_object_ordered_iter_end() to release the iterator
 * when you have finished iterating.
 */
bool ph_var_object_ordered_iter_first(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter,
    ph_string_t **key, ph_variant_t **val);

/** Continue iterating an object value
 *
 * You must call ph_var_object_ordered_iter_end() to release the iterator
 * when you have finished iterating.
 */
bool ph_var_object_ordered_iter_next(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter,
    ph_string_t **key, ph_variant_t **val);

/** Release ordered iterator resources
 */
void ph_var_object_ordered_iter_end(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter);

/** Compare two variants for equality
 *
 * * Two integer or real values are equal if their contained numeric values
 *   are equal.  An integer value is never equal to a real value.
 * * Two strings are equal if their bit strings are equal, byte by byte.
 * * Two arrays are equal if they have the same number of elements and each
 *   element in the first array is equal to the corresponding element in the
 *   second array.
 * * Two objects are equal if they have exactly the same keys and the value
 *   for each key in the first object is equal to the value of the corresponding
 *   key in the second object.
 * * Two true, false or null values are equal if their types are equal
 *
 * ph_var_equal() returns true if the values are equal, false otherwise.
 */
bool ph_var_equal(ph_variant_t *a, ph_variant_t *b);

#define PH_VAR_VALIDATE_ONLY 1
#define PH_VAR_STRICT 2

/** Build a new variant value based on a format string
 *
 * For each format character (except for `{}[]n`), one argument
 * is consumed and used to build the corresponding value.
 *
 * See [the section above on Building Values](#variant--Building-Values).
 *
 * Returns `NULL` on error.
 */
ph_variant_t *ph_var_pack(ph_var_err_t *error, const char *fmt, ...);

/** Build a new variant value based on a format string
 *
 * For each format character (except for `{}[]n`), one argument
 * is consumed and used to build the corresponding value.
 *
 * See [the section above on Building Values](#variant--Building-Values).
 *
 * Returns `NULL` on error.
 */
ph_variant_t *ph_var_vpack(ph_var_err_t *error, const char *fmt, va_list ap);

/** Parse a variant into native C values
 *
 * See [the section above on Parsing and Validating values
 * ](#variant--Parsing-and-Validating-Values).
 *
 * Returns `PH_OK` on success.
 */
ph_result_t ph_var_unpack(ph_variant_t *root, ph_var_err_t *error,
    uint32_t flags, const char *fmt, ...);

/** Parse a variant into native C values
 *
 * See [the section above on Parsing and Validating values
 * ](#variant--Parsing-and-Validating-Values).
 *
 * Returns `PH_OK` on success.
 */
ph_result_t ph_var_vunpack(ph_variant_t *root, ph_var_err_t *error,
    uint32_t flags, const char *fmt, va_list ap);

/** Evaluate a JSONPath style expression.
 *
 * libPhenom supports a limited subset of JSONPath; see [the start of this
 * document](#variant--JSONPath-style-queries) for more details on the supported syntax.
 *
 * Returns a borrowed reference on the matching element if found, else
 * returns NULL pointer.
 */
ph_variant_t *ph_var_jsonpath_get(ph_variant_t *var, const char *path);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


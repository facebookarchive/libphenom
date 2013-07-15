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

/**
 * # Variant Data Type
 *
 * Variants can represent runtime variable values and are primarily
 * useful when it comes to serialization to/from the JSON or BSER encoding
 * formats.
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

/** Delete a key/value pair
 */
ph_result_t ph_var_object_del(ph_variant_t *obj, ph_string_t *key);

/** Get the value for a given key
 *
 * Returns NULL if the key isn't present in the object, otherwise
 * returns a borrowed reference to the value.
 */
ph_variant_t *ph_var_object_get(ph_variant_t *obj, ph_string_t *key);

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


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


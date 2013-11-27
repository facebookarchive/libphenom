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

#ifndef PHENOM_HASHTABLE_H
#define PHENOM_HASHTABLE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * # Hash Table
 *
 * libPhenom provides a hash table facility that allows the
 * construction of maps from an arbitrary key type to
 * an arbitrary value type.
 *
 * The hash table uses closed hashing / open addressing to reduce the volume
 * of discrete allocations required to maintain the table.
 *
 * Note: The tables have no built-in mutex or locking capability.  For a
 * concurrent map you might consider using the Concurrency Kit hash-set API.
 * You may alternatively wrap your hash table implementation in an appropriate
 * mutex or reader-writer lock.
 *
 * ## Defining a hash table
 *
 * This implementation uses separate key and value definition structures
 * so that you can factor out and re-use the code for keys
 * across different hash tables.  `ph_string_t*` key and value implementations
 * are provided:
 *
 * ```
 * ph_ht_t ht;
 *
 * // Declare a map from ph_string_t* -> ph_string_t*
 * ph_ht_init(&ht, 1, &ph_ht_string_key_def, &ph_ht_string_val_def);
 * ```
 *
 * The table allocates storage for each element based on the sum of
 * the key size and value size declared in the key and value definitions.
 *
 * The implementation is flexible enough to allow you to track pointers
 * to other structures or to embed them directly, provided that they
 * have a fixed size.
 *
 * ## Inserting data into the table
 *
 * All insert operations are expressed in terms of the ph_ht_insert()
 * function.  Two convenience functions are provided to make this less
 * verbose: ph_ht_set() and ph_ht_replace().
 *
 * Since the API is intended to handle arbitrary key or data types, it
 * is expressed in terms of `void*` types for keys and values.
 *
 * Note: you must pass the address of the keys and values
 * to be stored.
 *
 * For example, continuing with our string->string map above:
 *
 * ```COUNTEREXAMPLE
 * ph_string_t *key = ph_string_make_cstr(mt_mytype, "key");
 * ph_string_t *val = ph_string_make_cstr(mt_mytype, "val");
 *
 * ph_ht_set(&ht, key, val);   // WRONG! will crash
 * ```
 *
 * ```
 * ph_string_t *key = ph_string_make_cstr(mt_mytype, "key");
 * ph_string_t *val = ph_string_make_cstr(mt_mytype, "val");
 *
 * ph_ht_set(&ht, &key, &val); // CORRECT!
 *
 * // the set and replace APIs operate in COPY mode, so we need to
 * // clean up our string references
 * ph_string_delref(key);
 * ph_string_delref(val);
 * ```
 *
 * ## Inserting and claiming values
 *
 * If you're constructing refcounted keys/values just to put them in the
 * table, it can be tedious to repitively bracket the inserts with
 * create and delref calls.
 *
 * For this situation you can use ph_ht_insert() and the `PH_HT_CLAIM`
 * flag.  When claiming, if the insertion was successful, the table will
 * take ownership of the references you passed to the table.  If the insert
 * fails, you still own your references so that you can print out a
 * meaningful error.
 *
 * ```
 * ph_string_t *key = ph_string_make_cstr(mt_mytype, "key");
 * ph_string_t *val = ph_string_make_cstr(mt_mytype, "val");
 *
 * if (ph_ht_insert(&ht, &key, &val, PH_HT_CLAIM) != PH_OK) {
 *   ph_log(PH_LOG_ERR, "failed to insert `Ps%p -> `Ps%p",
 *         (void*)key, (void*)val);
 *   ph_string_delref(key);
 *   ph_string_delref(val);
 * }
 * ```
 *
 * Pragmatic commentary: if you know that your insert operation won't
 * collide with an existing key (or you are using `PH_HT_REPLACE`),
 * and your hash table is large enough (see ph_ht_grow()), the insert
 * operation cannot fail and you may elect to ignore the result of the
 * insert.  We won't judge you.
 *
 * ## Fetching Data by Key
 *
 * There are two functions for retrieving data from the table.  ph_ht_get()
 * returns the raw pointer to the value as stored in the table whereas
 * ph_ht_lookup() copies the value into a buffer that you provide.
 *
 * The differences are best illustrated by continuing our example:
 *
 * ```
 * ph_string_t **via_get;
 * ph_string_t *via_lookup;
 *
 * // Need to de-reference this, as we're pointing at the value in
 * // the internal table element
 * via_get = ph_ht_get(&ht, &key);
 * if (via_get) {
 *    ph_log(PH_LOG_DEBUG, "got `Ps%p", *via_get);
 * }
 *
 * if (ph_ht_lookup(&ht, &key, &via_lookup, true) == PH_OK) {
 *    ph_log(PH_LOG_DEBUG, "got `Ps%p", via_lookup);
 *    ph_string_delref(via_lookup);
 * }
 * ```
 *
 * ## Iterating the table
 *
 * Two different iterators are provided; the first is a light weight
 * iterator that passes over the table contents in their physical order.
 * The second is a heavier weight ordered iterator that performs a sort
 * of the keys and then passes over the table in sorted key order.
 *
 * Both iterators are safe in the presence of concurrent delete operations,
 * but exhibit undefined behavior if the table is resized.
 *
 * ```
 * ph_ht_iter_t iter;
 * ph_ht_string **key, **val;
 *
 * // Lightweight iterator, order is undefined
 * if (ph_ht_iter_first(&ht, &iter, (void*)&key, (void*)&val)) do {
 *    ph_log(PH_LOG_DEBUG, "`Ps%p -> `Ps%p", *key, *val);
 * } while (ph_ht_iter_next(&ht, &iter, (void*)&key, (void*)&val));
 * ```
 *
 * ```
 * ph_ht_ordered_iter_t iter;
 * ph_ht_string **key, **val;
 *
 * // Iterate in sorted key order.  Slower than the unordered iteration
 * // as it has to sort the keys at the start and then look up the value
 * // on each iteration
 * if (ph_ht_ordered_iter_first(&ht, &iter, (void*)&key, (void*)&val)) do {
 *    ph_log(PH_LOG_DEBUG, "`Ps%p -> `Ps%p", *key, *val);
 * } while (ph_ht_ordered_iter_next(&ht, &iter, (void*)&key, (void*)&val));
 * // Release sorted keys
 * ph_ht_ordered_iter_end(&ht, &iter);
 * ```
 *
 */

/** hash table key definition
 *
 * This structure defines the size, hashing, comparison and memory
 * management of keys for a hash table instance.
 */
struct ph_ht_key_def {
  /* size of keys in bytes */
  uint32_t ksize;

  /* Hash function */
  uint32_t (*hash_func)(const void *key);

  /* function to compare two keys.
   * This is used both for equality and ordering comparisons.
   * In the latter case, this function is passed as the compare
   * function parameter to qsort().
   *
   * Receives the address of each key.
   *
   * If NULL, memcmp is used for maintaining the hash table,
   * but the ordered_iter can not be used.
   * */
  int (*key_compare)(const void *a, const void *b);

  /* function to copy a key
   * This is called to populate the key portion
   * of an element during insertion, and also when using lookup.
   *
   * If you are tracking refcounted pointers, this is the logical
   * place to perform an addref operation.
   *
   * If NULL, memcpy is used */
  bool (*key_copy)(const void *src, void *dest);

  /* function to free a key
   * Called when deleting an entry.
   *
   * If you are tracking refcounted pointers, this is the logical
   * place to perform a delref operation.
   *
   * May be NULL */
  void (*key_delete)(void *key);
};

/** hash table value definition
 *
 * This structure defines the size and memory management of values
 * for a hash table instance.
 */
struct ph_ht_val_def {
  /* size of values in bytes */
  uint32_t vsize;

  /* function to copy a value
   * This is called to populate the value portion
   * of a element during insertion.
   *
   * If you are tracking refcounted pointers, this is the logical
   * place to perform an addref operation.
   *
   * If NULL, memcpy is used */
  bool (*val_copy)(const void *src, void *dest);

  /* function to free a value
   * Called when replacing or deleting an entry.
   *
   * If you are tracking refcounted pointers, this is the logical
   * place to perform a delref operation.
   *
   * May be NULL */
  void (*val_delete)(void *val);
};

#define PH_HT_ELEM_EMPTY     0
#define PH_HT_ELEM_TAKEN     1
#define PH_HT_ELEM_TOMBSTONE 2
struct ph_ht_elem {
  uint32_t status;
  /* variable size: key followed by value embedded here */
};

struct ph_ht {
  uint32_t nelems;
  uint64_t table_size, elem_size, mask;
  const struct ph_ht_key_def *kdef;
  const struct ph_ht_val_def *vdef;
  /* points to the table, an array of table_size elements */
  char *table;
};

typedef struct ph_ht ph_ht_t;

/** Initialize a hash table
 *
 * Configures the table pointed to by `ht` to using the supplied
 * definition and sized to accomodate at least `size_hint` entries
 *
 * Returns `PH_OK` on success, or an error code on failure.
 */
ph_result_t ph_ht_init(ph_ht_t *ht, uint32_t size_hint,
    const struct ph_ht_key_def *kdef,
    const struct ph_ht_val_def *vdef);

/** Grow the table to accomodate nelems
 *
 * If you know that you will be inserting a number of elements that
 * would trigger more than a single resize of the table (for example,
 * moving from a small power of two elements through a couple of powers
 * of two), it is advantageous to pre-allocate the ultimate size to
 * avoid reallocating and rebuilding the table multiple times.
 *
 * If `nelems` is larger than the current capacity of the table,
 * this function will grow and rebuild the table to the ideal size
 * for `nelems`.
 *
 * Returns true if the table is can accomodate nelems, false if there
 * was an issue in growing and rebuilding the table.
 */
bool ph_ht_grow(ph_ht_t *ht, uint32_t nelems);

/** Tear down a hash table
 *
 * Frees all elements and the table portion.
 */
void ph_ht_destroy(ph_ht_t *ht);

/** Empty the hash table
 *
 * Frees all elements but retains the table definition and current
 * size, ready for re-use.
 */
void ph_ht_free_entries(ph_ht_t *ht);

/** Set, but not replace, an entry in the table
 *
 * Equivalent to calling ph_ht_insert() with the flags set to
 * `PH_HT_NO_REPLACE|PH_HT_COPY`.
 */
ph_result_t ph_ht_set(ph_ht_t *ht, void *key, void *value);

/** Set or replace an entry in the table
 *
 * Equivalent to calling ph_ht_insert() with the flags set to
 * `PH_HT_REPLACE|PH_HT_COPY`.
 */
ph_result_t ph_ht_replace(ph_ht_t *ht, void *key, void *value);

#define PH_HT_REPLACE    1
#define PH_HT_NO_REPLACE 0
#define PH_HT_CLAIM_KEY 2
#define PH_HT_COPY_KEY  0
#define PH_HT_CLAIM_VAL 4
#define PH_HT_COPY_VAL  0
#define PH_HT_CLAIM (PH_HT_CLAIM_KEY|PH_HT_CLAIM_VAL)
#define PH_HT_COPY  (PH_HT_COPY_KEY|PH_HT_COPY_VAL)
/** Insert an entry in the table
 *
 * `key` is the address of the key, `value` is the address of the value.
 *
 * Attempts to allocate space as needed; this may fail and cause the insert
 * operation to return `PH_NOMEM`.
 *
 * If `key` would collide with an existing entry:
 *
 * * if flags contains `PH_HT_NO_REPLACE`,
 *   this function will return `PH_EXISTS`
 * * if flags contains `PH_HT_REPLACE`,
 *   the del_val function is invoked for the currently
 *   associated value prior to replacing the value.
 *
 * If the insertion is creating this entry, rather than replacing:
 *
 * * if flags contains `PH_HT_COPY_KEY`, the copy_key function
 *   defined for the key type for this hash table will be used to
 *   obtain a copy of the key for the hash table.
 * * if flags contains `PH_HT_CLAIM_KEY`, a default memcpy
 *   operation will be used to copy the key into the table,
 *   claiming ownership.
 *
 * When populating the value:
 *
 * * if flags contains `PH_HT_COPY_VAL`, the copy_val function
 *   defined for the value type for this hash table will be used to
 *   obtain a copy of the value for the hash table.
 * * if flags contains `PH_HT_CLAIM_VAL`, a default memcpy
 *   operation will be used to copy the value into the table,
 *   claiming ownership.
 *
 * You may use `PH_HT_COPY` as a shortcut for setting both
 * `PH_HT_COPY_VAL` and `PH_HT_COPY_KEY` in flags.
 *
 * You may use `PH_HT_CLAIM` as a shortcut for setting both
 * `PH_HT_CLAIM_VAL` and `PH_HT_CLAIM_KEY` in flags.
 *
 * If flags contains both `PH_HT_COPY_VAL` and `PH_HT_CLAIM_VAL`
 * the results of calling this function are undefined.
 *
 * If flags contains both `PH_HT_COPY_KEY` and `PH_HT_CLAIM_KEY`
 * the results of calling this function are undefined.
 *
 * If flags contains both `PH_HT_REPLACE` and `PH_HT_NO_REPLACE`
 * the results of calling this function are undefined.
 *
 * When successful, returns `PH_OK`.
 */
ph_result_t ph_ht_insert(ph_ht_t *ht, void *key, void *value, int flags);

/** Looks up the value associated with key and returns its address.
 * Returns 0 if there was no matching value.
 *
 * This function will NOT invoke copy_val on the returned value.
 */
void *ph_ht_get(ph_ht_t *ht, const void *key);

/** Looks up the value associated with key.
 * If found, stores the value into *VAL.
 * If copy==true and copy_val is defined, then it is invoked on the value
 * prior to storing it to *VAL.
 * Returns `PH_OK` if the value was found, `PH_NOENT` if it was not present,
 * or an error code in case the value could not be copied.
 */
ph_result_t ph_ht_lookup(ph_ht_t *ht, const void *key, void *val, bool copy);

/** Deletes the value associated with key.
 * If del_val is defined, it is invoked on the value stored in the table.
 * If del_key is defined, it is invoked on the key stored in the table.
 * Returns `PH_OK` if the element was present in the table and was removed,
 * `PH_NOENT` otherwise.
 */
ph_result_t ph_ht_del(ph_ht_t *ht, const void *key);

/** Returns the number of elements stored in the table */
uint32_t ph_ht_size(ph_ht_t *ht);

/** Iterator for hash elements.
 * Iteration will be halted if the table size changes during
 * iteration. */
struct ph_ht_iter {
  uint32_t slot;
  uint32_t size;
};
typedef struct ph_ht_iter ph_ht_iter_t;

/** Begin iterating a table
 *
 * Returns false if the table is empty, otherwise updates the
 * iter such that a subsequent call to ph_ht_iter_next will
 * retrieve the next value, and stores the address of the
 * key and value of this first element in the provided pointers.
 *
 * The order of iteration is undefined.  For a defined iteration
 * order, use ph_iter_ordered_iter_first().
 */
bool ph_ht_iter_first(ph_ht_t *ht, ph_ht_iter_t *iter, void **key, void **val);

/** Walk to the next entry in a table
 *
 * Returns false if there are no more elements to iterate.
 * Otherwise, updates the iter such that a subsequent call
 * will retrieve the next value, and stores the address of
 * the key and value of the current element in the provided
 * pointers.
 */
bool ph_ht_iter_next(ph_ht_t *ht, ph_ht_iter_t *iter, void **key, void **val);

/** Iterating in a defined order
 *
 * This iterator performs a sort over the keys of the table and maintains
 * a copy of the sorted keys.
 */
struct ph_ht_ordered_iter {
  uint32_t slot;
  uint32_t size;
  char *slots;
};
typedef struct ph_ht_ordered_iter ph_ht_ordered_iter_t;

/** Begin iterating a table in a defined order.
 *
 * If the table is empty, returns false and sets errno to ENOENT.
 *
 * Otherwise, allocates storage to hold keys of the table.  If the
 * allocation fails, returns false and sets errno to ENOMEM.
 *
 * Uses the key_compare function to sort the keys and stores the
 * address of the key and value of the first logical key/value
 * into the provided pointers, and returns true.
 *
 * The caller *must* call ph_ht_ordered_iter_end() to dispose of
 * the resources allocated by the iterator when it is no longer
 * needed.
 */
bool ph_ht_ordered_iter_first(ph_ht_t *ht, ph_ht_ordered_iter_t *iter,
    void **key, void **val);

/** Walk to the next ordered entry in a table
 *
 * Returns false if there are no more elements to iterate.
 *
 * Otherwise, updates the iter such that a subsequent call will
 * retrieve the next value and stores the address of the key and value
 * of the current element in the provided pointers.
 *
 * The caller *must* call ph_ht_ordered_iter_end() to dispose of
 * the resources allocated by the iterator when it is no longer needed.
 */
bool ph_ht_ordered_iter_next(ph_ht_t *ht, ph_ht_ordered_iter_t *iter,
    void **key, void **val);

/** Release resources associated with an ordered iterator */
void ph_ht_ordered_iter_end(ph_ht_t *ht, ph_ht_ordered_iter_t *iter);

/* String key definition for hash tables */
extern struct ph_ht_key_def ph_ht_string_key_def;

/* String value definition for hash tables */
extern struct ph_ht_val_def ph_ht_string_val_def;

/* Generic pointer value definition for hash tables */
extern struct ph_ht_val_def ph_ht_ptr_val_def;

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


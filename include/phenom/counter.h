/*
 * Copyright 2012-2013 Facebook, Inc.
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
 * # Counters
 * The counter subsystem provides a set of functions that
 * allows the application to build a hierarchy of 64-bit
 * counter values.
 *
 * These values may be modified and queried atomically via the provided API.
 *
 * Functions are provided to introspect the hierarchy and groups of
 * related counters can be read consistently.  Note that the system does
 * not provide a means for snapshotting the entire counter hierarchy.
 *
 * Counters are implemented such that individual threads may
 * manipulate their values uncontested (with no locking!), but allowing
 * for a reader to obtain a consistent view of a related set of counters.
 *
 * ## Overview
 *
 * First establish the counter scope, and register counter nodes within
 * it:
 *
 * ```
 * ph_counter_scope_t *myscope = ph_counter_scope_define(NULL, "myscope", 8);
 * const char *names[2] = {"sent", "recvd"};
 * #define MY_SENT_COUNTER_SLOT 0
 * #define MY_RECVD_COUNTER_SLOT 1
 * ph_counter_scope_register_counter_block(myscope, 2, 0, names);
 * ```
 *
 * Then, elsewhere in you program, adjust the counter values.  There are
 * two interfaces for this; one is the slow and convenient way:
 *
 * ```
 * // Bump the sent counter by 1.  Slower and more convenient
 * ph_counter_scope_add(myscope, MY_SENT_COUNTER_SLOT, 1);
 * ```
 *
 * The preferred approach is to open a handle to your thread-local
 * counter block.  This is ideal if you are driving your thread and
 * can maintain that block handle in a local variable.  Alternatively,
 * if you are running a tight loop or are updating a series of counters,
 * you should prefer to open the block in this way.  **Note that the block
 * returned can only have its counters modified on the same thread that
 * obtained the block**:
 *
 * ```
 * ph_counter_block_t *block = ph_counter_block_open(myscope);
 * // Bump the sent counter by 1
 * ph_counter_block_add(block, MY_SENT_COUNTER_SLOT, 1);
 * ```
 *
 * To make an update to multiple counters at once:
 *
 * ```
 * uint8_t slots[2] = { MY_SENT_COUNTER_SLOT, MY_RECVD_COUNTER_SLOT };
 * int64_t values[2] = { 4, 5 };
 * // adds 4 to the sent counter and 5 to the recvd counter
 * ph_counter_block_bulk_add(block, 2, slots, values);
 * ```
 */

#ifndef PHENOM_COUNTER_H
#define PHENOM_COUNTER_H

#include <phenom/defs.h>
#include <ck_sequence.h>
#include "phenom/refcnt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_counter_scope;
struct ph_counter_block;
typedef struct ph_counter_scope ph_counter_scope_t;
typedef struct ph_counter_block ph_counter_block_t;

/** Defines a new counter scope.
 *
 * Counter scopes contain one or more counter values that
 * are logically grouped together.
 *
 * The counter subsystem provides fast update access to the
 * individual counter values and a mechanism for reading a
 * consistent snapshot of the counter values in the scope.
 *
 * * `parent` - optional relative counter scope. May be NULL.
 * * `path` - scope name for this set of counters. Must not be NULL.
 * * `max_counters` - hint as to the max number of counters that must
 *    be available for use in this counter scope.  The system
 *    only guarantees that this many slots are available, which
 *    may restrict the number of counters that can be dynamically
 *    added later.
 *
 * Returns a counter scope instance.  The caller owns a reference
 * to this instance and must release it via ph_counter_scope_delref()
 * when it is no longer needed.
 */
ph_counter_scope_t *ph_counter_scope_define(
    ph_counter_scope_t *parent,
    const char *path,
    uint8_t max_counters);

/** Resolve a named counter scope
 *
 * Given a relative scope and name, resolves it to a scope instance.
 *
 * * `parent` - optional relative counter scope. May be NULL.
 * * `path` - scope name to be resolved. Must not be NULL.
 *
 * Returns a counter scope instance.  The caller owns a reference
 * to this instance and must release it via ph_counter_scope_delref().
 */
ph_counter_scope_t *ph_counter_scope_resolve(
    ph_counter_scope_t *parent,
    const char *path);

/** Release a reference to a counter scope.
 *
 * When no more references to a scope remain, it is destroyed.
 * To delete a counter scope from the counter subsystem, it must be
 * unlinked from its parent scope (not currently supported).
 *
 * * `scope` - the scope to be released
 */
void ph_counter_scope_delref(ph_counter_scope_t *scope);

#define PH_COUNTER_INVALID 0xff

/** Registers a counter name in a counter scope.
 *
 * Returns a counter slot offset or `PH_COUNTER_INVALID`
 *
 * * `scope` - the scope in which the counter should be registered
 * * `name` - the name of the counter
 *
 * It is an error to register a counter with the same name as a child
 * scope.
 *
 * If the counter cannot be registered, returns `PH_COUNTER_INVALID`.
 */
uint8_t ph_counter_scope_register_counter(
    ph_counter_scope_t *scope,
    const char *name);

/** Registers a block of counter names and values in a counter scope.
 *
 * Returns true if the registration matched the desired slots, false otherwise
 *
 * * `scope` - the scope in which the counters should be registered.
 * * `num_slots` - the number of slots being registered
 * * `first_slot` - the desired slot value for the first slot being registered
 * * `slot_names` - an array of slot name strings to be registered.
 *
 * This function registers a set of counter names in the specified scope.
 * For example, if you have a set of counters, say "sent" and "recvd" and
 * you want to register them, you'd write something like:

```
const char *names[2] = {"sent", "recvd"};
ph_counter_scope_register_counter_block(scope, 2, 0, names);
```

 *
 * If the requested first_slot is not yet allocated, then it will allocate
 * the counters to the names you've specified and return true.  Otherwise
 * it will have no side effects but return false.
 *
 * If the scope doesn't have enough room for the num_slots, returns false.
 */
bool ph_counter_scope_register_counter_block(
    ph_counter_scope_t *scope,
    uint8_t num_slots,
    uint8_t first_slot,
    const char **names
);

/** Modify a counter value.
 *
 * Adds the specified value to the current counter value.
 * Note that you may add a negative counter value to decrement it.
 *
 * * `scope` - the scope of the counter
 * * `offset` - the counter slot offset
 * * `value` - the value to add to the current counter value.
 */
void ph_counter_scope_add(
    ph_counter_scope_t *scope,
    uint8_t offset,
    int64_t value);

/** Open a handle on the set of counters for the current thread
 *
 * The handle is useful in two main situations:
 *
 * 1. The same thread is making very frequent updates to counter
 *    values and wants to shave off the TLS overheads.
 *    See ph_counter_block_add().
 * 2. The same thread is updating multiple counters in this scope
 *    and wants to do so as efficiently as possible.
 *    See ph_counter_block_bulk_add().
 *
 * The returned handle is reference counted and can be released
 * from any thread, but it changing the stats within it is only
 * safe when carried out by the owning thread.
 *
 * You must call ph_counter_block_delref() on the handle when
 * it is no longer needed.
 *
 * * `scope` - the scope to open
 *
 * Returns a thread local counter block.
 */
ph_counter_block_t *ph_counter_block_open(
    ph_counter_scope_t *scope);

struct ph_counter_block {
  /* must be first in the struct so that we can cast the return
   * value from ck_hs_get() */
  uint32_t scope_id;
  ph_refcnt_t refcnt;
  uint32_t seqno CK_CC_CACHELINE;
  char pad[CK_MD_CACHELINE - sizeof(uint32_t)];

  /* variable size array; the remainder of this struct
   * holds num_slots elements */
  int64_t slots[1];
};

static inline void ph_counter_block_record_write(
    ph_counter_block_t *block)
{
  block->seqno += 2;
}

/** Modify a counter value in a thread local block
 *
 * Adds the specified value to the current counter value.
 * Note that you may add a negative counter value to decrement it.
 *
 * * `block` - the block containing the counters
 * * `offset` - the counter slot offset
 * * `value` - the value to add to the current counter value.
 */
static inline void ph_counter_block_add(
    ph_counter_block_t *block,
    uint8_t offset,
    int64_t value)
{
  block->slots[offset] += value;
  ph_counter_block_record_write(block);
}

/** Release a counter block
 *
 * * `block` - the block to be released.
 */
void ph_counter_block_delref(
    ph_counter_block_t *block);

/** Modify a set of counters in a block
 *
 * Updates multiple slots in a single write "transaction".
 * Counters use a sequence "lock" to allow a reader to detect
 * an inconsistent read.  This function places the set of counter
 * updates under the same write sequence, making it both marginally
 * faster to update a set of counters in the same block and faster
 * for a reader to detect and retry a block read when obtaining
 * the counter view.
 *
 * If you frequently update a set of related counters together,
 * it is recommended that you use this interface.
 *
 * * `block` - the counter block for this thread
 * * `num_slots` - the number of slots being manipulated
 * * `slots` - an array of num_slots slot offsets
 * * `values` - an array of num_slots values
 *
 * NOTE: passing an invalid slot offset leads to undefined behavior.
 * It is the callers responsibility to ensure that the offsets are
 * valid.
 */
static inline void ph_counter_block_bulk_add(
    ph_counter_block_t *block,
    uint8_t num_slots,
    const uint8_t *slots,
    const int64_t *values)
{
  int i;

  block->seqno++;
  for (i = 0; i < num_slots; i++) {
    block->slots[slots[i]] += values[i];
  }
  block->seqno++;
}


/** Returns a current counter value.
 *
 * Returns the current value associated with a counter slot in the
 * specified scope.
 *
 * If the offset is invalid, returns 0.
 *
 * * `scope` - the containing scope
 * * `offset` - the counter slot offset
 */
int64_t ph_counter_scope_get(
    ph_counter_scope_t *scope,
    uint8_t offset);

/** Returns a consistent view on the counters in a scope.
 *
 * Since counters may be atomically modified from any thread, it can
 * be difficult to make sense of the set of related counters.
 * The counter subsystem uses a sequence synchronization
 * mechanism that allows counter updates to proceed unimpeded but that
 * allows readers to determine whether they have a consistent view.
 * The get_view operation will retry its counter reads until is
 * has a consistent view on that set of counters.
 *
 * If the names parameter is specified, it is an array of size num_slots
 * that will be populated with the names of the counters registered
 * in the scope.
 *
 * Returns the number of slots populated.
 *
 * * `scope` - the containing scope
 * * `num_slots` - the number of slots to read
 * * `slots` - an array of size num_slots to hold the values
 * * `names` - an optional array to receive the counter names
 */
uint8_t ph_counter_scope_get_view(
    ph_counter_scope_t *scope,
    uint8_t num_slots,
    int64_t *slots,
    const char **names);

/** Returns the fully qualified name of the counter scope
 *
 * Introspects the provided scope and returns its full path name.
 *
 * Returns the name.
 *
 * The returned string is only valid while the scope reference is
 * maintained; once you delref, the name pointer value is undefined.
 */
const char *ph_counter_scope_get_name(
    ph_counter_scope_t *scope);

/** Returns the number of allocated slots in a scope.
 *
 * Introspects the provided scope and returns the number of slots
 * that have been allocated.  This is useful when dynamically examining
 * a scope to render the counter values.
 *
 * * `scope` - the scope being inspected
 *
 * Returns the number of allocated slots
 */
uint8_t ph_counter_scope_get_num_slots(
    ph_counter_scope_t *scope);

/* an iterator for scopes */
struct ph_counter_scope_iterator {
  void *ptr;
  intptr_t offset;
};
typedef struct ph_counter_scope_iterator ph_counter_scope_iterator_t;

/** Initialize a scope iterator.
 *
 * * `iter` - the iterator to be initialized
 */
void ph_counter_scope_iterator_init(
    ph_counter_scope_iterator_t *iter);

/** iterate scopes
 *
 * * `iter` - the iterator
 *
 * Returns The next scope in the iteration, or NULL if the end has been
 * reached.
 *
 * Iteration is thread safe; no locks are required, but iteration may not
 * see scopes that were defined partway through iteration.
 *
 * Iteration order is undefined.
 *
 * The caller is responsible for calling ph_counter_scope_delref()
 * on the returned scope.
 *
 * If you wish to halt iteration early, simply break out of your loop.
 * The iterator does not hold any resources and does not need to be
 * destroyed.
 */
ph_counter_scope_t *ph_counter_scope_iterator_next(
    ph_counter_scope_iterator_t *iter);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


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

/**
 * The phenom counter subsystem provides a set of functions that
 * allows the application to build a hierarchy of 64-bit
 * counter values.
 *
 * These values may be modified and queried atomically via the provided API.
 *
 * Functions are provided to introspect the hierarchy and groups of
 * related counters can be read consistently.  Note that the system does
 * not provide a means for snapshotting the entire counter hierarchy.
 *
 * Note that this doesn't replace the common/stats functionality that
 * is used by other C++/Thrift service implementations; it is present
 * primarily to record memory stats in an efficient way.
 * These can then be exported via Thrift if desired.
 */

#ifndef PHENOM_COUNTER_H
#define PHENOM_COUNTER_H

#include <phenom/defs.h>

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
 * @param parent optional relative counter scope. May be NULL.
 * @param path scope name for this set of counters. Must not be NULL.
 * @param max_counters hint as to the max number of counters that must
 *        be available for use in this counter scope.  The system
 *        only guarantees that this many slots are available, which
 *        may restrict the number of counters that can be dynamically
 *        added later.
 *
 * @return a counter scope instance.  The caller owns a reference
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
 * @param parent option relative counter scope. May be NULL.
 * @param path scope name to be resolved. Must not be NULL.
 * @return a counter scope instance.  The caller owns a reference
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
 * @param scope the scope to be released
 */
void ph_counter_scope_delref(ph_counter_scope_t *scope);

#define PH_COUNTER_INVALID 0xff

/** Registers a counter name in a counter scope.
 *
 * @returns a counter slot offset or PHENOM_COUNTER_INVALID
 * @param scope the scope in which the counter should be registered
 * @param name the name of the counter
 *
 * It is an error to register a counter with the same name as a child
 * scope.
 *
 * If the counter cannot be registered, returns PHENOM_COUNTER_INVALID.
 */
uint8_t ph_counter_scope_register_counter(
    ph_counter_scope_t *scope,
    const char *name);

/** Registers a block of counter names and values in a counter scope.
 *
 * @returns true if the registration matched the desired slots, false otherwise
 * @param scope the scope in which the counters should be registered.
 * @param num_slots the number of slots being registered
 * @param first_slot the desired slot value for the first slot being registered
 * @param slot_names an array of slot name strings to be registered.
 *
 * This function registers a set of counter names in the specified scope.
 * For example, if you have a set of counters, say "sent" and "recvd" and
 * you want to register them, you'd write something like:
\code
const char *names[2] = {"sent", "recvd"};
ph_counter_scope_register_counter_block(scope, 2, 0, names);
\endcode
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
 * @param scope the scope of the counter
 * @param slot the counter slot offset
 * @param value the value to add to the current counter value.
 */
void ph_counter_scope_add(
    ph_counter_scope_t *scope,
    uint8_t offset,
    int64_t value);

/** Open a handle on the set of counters for the current thread
 *
 * The handle is useful is two main situations:
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
 * @param scope the scope to open
 * @return a thread local counter block.
 */
ph_counter_block_t *ph_counter_block_open(
    ph_counter_scope_t *scope);

/** Modify a counter value in a thread local block
 *
 * Adds the specified value to the current counter value.
 * Note that you may add a negative counter value to decrement it.
 *
 * @param block the block containing the counters
 * @param slot the counter slot offset
 * @param value the value to add to the current counter value.
 */
void ph_counter_block_add(
    ph_counter_block_t *block,
    uint8_t offset,
    int64_t value);

/** Release a counter block
 *
 * @param block the block to be released.
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
 * @param block the counter block for this thread
 * @param num_slots the number of slots being manipulated
 * @param slots an array of num_slots slot offsets
 * @param values an array of num_slots values
 *
 * NOTE: passing an invalid slot offset leads to undefined behavior.
 * It is the callers responsibility to ensure that the offsets are
 * valid.
 */
void ph_counter_block_bulk_add(
    ph_counter_block_t *block,
    uint8_t num_slots,
    const uint8_t *slots,
    const int64_t *values);

/** Returns a current counter value.
 *
 * @returns the current value associated with a counter slot in the
 * specified scope.
 *
 * If the offset is invalid, returns 0.
 *
 * @param scope the containing scope
 * @param slot the counter slot offset
 */
int64_t ph_counter_scope_get(
    ph_counter_scope_t *scope,
    uint8_t offset);

/** Returns a consistent view on the counters in a scope.
 *
 * Since counters may be atomically modified from any thread, it can
 * be difficult to make sense of the set of related counters.
 * The phenom counter subsystem uses a sequence synchronization
 * mechanism that allows counter updates to proceed unimpeded but that
 * allows readers to determine whether they have a consistent view.
 * The get_view operation will retry its counter reads until is
 * has a consistent view on that set of counters.
 *
 * If the names parameter is specified, it is an array of size num_slots
 * that will be populated with the names of the counters registered
 * in the scope.
 *
 * @returns the number of slots populated.
 * @param scope the containing scope
 * @param num_slots the number of slots to read
 * @param slots an array of size num_slots to hold the values
 * @param names an optional array to receive the counter names
 */
uint8_t ph_counter_scope_get_view(
    ph_counter_scope_t *scope,
    uint8_t num_slots,
    int64_t *slots,
    const char **names);

/** Returns the fully qualified name of the counter scope
 *
 * Introspects the provided scope and returns its full path name.
 * @return the name
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
 * @param scope the scope being inspected
 * @return the number of allocated slots
 */
uint8_t ph_counter_scope_get_num_slots(
    ph_counter_scope_t *scope);

/** an iterator for scopes */
struct ph_counter_scope_iterator {
  void *ptr;
  uint64_t offset;
};
typedef struct ph_counter_scope_iterator ph_counter_scope_iterator_t;

/** Initialize a scope iterator.
 * @param iter the iterator to be initialized
 */
void ph_counter_scope_iterator_init(
    ph_counter_scope_iterator_t *iter);

/** iterate scopes
 * @param iter the iterator
 * @returns The next scope in the iteration, or NULL if the end has been
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


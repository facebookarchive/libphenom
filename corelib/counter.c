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

#include "phenom/counter.h"
#include "phenom/refcnt.h"
#include <ck_sequence.h>
#include <ck_ht.h>
#include <ck_stack.h>
#include <ck_spinlock.h>

/* Counters are stored using a low-contention scheme that allows
 * counter updates to proceed uncontested for any given thread.
 *
 * For each defined counter scope, each thread maintains a block
 * of counter values.  The counter block is protected by a seqlock;
 * a stat read operation will walk the counter blocks for each
 * thread and sum them into the stat structure that is to be
 * returned.
 *
 * Because we may increment and decrement across threads, the value
 * of any given counter on any given thread at a certain point in
 * time may be negative even though the total is positive.
 */

struct phenom_counter_block {
  phenom_refcnt_t refcnt;
  ck_sequence_t seqlock;

  /* variable size array; the remainder of this struct
   * holds num_slots elements */
  int64_t slots[1];
};

struct phenom_counter_scope {
  phenom_refcnt_t refcnt;
  uint32_t scope_id;
  ck_ht_hash_t hash;

  uint8_t num_slots, next_slot;
  /* points to just after slot_names */
  char *scope_name;
  char *full_scope_name;

  /* variable size array; the remainder of this struct
   * holds num_slots elements */
  char *slot_names[1];
};

/* Each thread maintains an instance of counter head.
 * It tracks the thread local counter_block structures
 */
struct phenom_counter_head {
  /* linkage so that a stat reader can find all blocks */
  ck_stack_entry_t stack_entry;
  /* maps counter_id to a block instance in this thread */
  ck_ht_t ht;
};

/* This defines a function called phenom_counter_head_from_stack_entry
 * that maps a ck_stack_entry_t to a phenom_counter_head */
CK_STACK_CONTAINER(struct phenom_counter_head,
    stack_entry, phenom_counter_head_from_stack_entry)

/** map of fully qualified name to scope.
 * The CK hash table is single producer, multi-consumer,
 * meaning that we only need to serialize write operations
 * on the hash table; reads can proceed safely without a lock. */
static ck_ht_t scope_map;
static ck_spinlock_t scope_map_lock = CK_SPINLOCK_INITIALIZER;

/** key to TLS for finding counter block instances */
static pthread_key_t tls_key;
static pthread_once_t done_tls_init = PTHREAD_ONCE_INIT;
static ck_stack_t all_heads = CK_STACK_INITIALIZER;

/** atomic scope identifier */
static uint32_t next_scope_id = 0;

static void phenom_counter_tls_dtor(void *ptr)
{
  struct phenom_counter_head *head = ptr;

  unused_parameter(head);

  // TODO: mark it dead, reapable
}

static void *ht_malloc(size_t size)
{
  return malloc(size);
}

static void ht_free(void *p, size_t b, bool r)
{
  unused_parameter(b);
  unused_parameter(r);

  free(p);
}

static struct ck_malloc ht_allocator = {
  .malloc = ht_malloc,
  .free = ht_free
};


static void phenom_counter_init(void)
{
  pthread_key_create(&tls_key, phenom_counter_tls_dtor);

  if (sizeof(struct phenom_counter_scope_iterator) !=
      sizeof(struct ck_ht_iterator)) {
    /* ideally, we'd let the compiler catch this, but we
     * don't want to pollute phenom_counter.h with the CK
     * functions, because C++ compilers hate it.
     * If you're seeing this abort trigger, you need to
     * update struct phenom_counter_scope_iterator to
     * have the same size as struct ck_ht_iterator */
    abort();
  }

  if (!ck_ht_init(&scope_map, CK_HT_MODE_BYTESTRING, NULL,
      &ht_allocator, 65536, lrand48())) {
    // If this fails, we're screwed
    abort();
  }
}

/** Determine whether a given counter name is acceptable.
 * We only check during definition.
 */
static bool is_valid_name(const char *name)
{
  if (!*name) return false;
  for (; *name; name++) {
    int c = *name;
    if (c >= 'a' && c <= 'z') continue;
    if (c >= 'A' && c <= 'Z') continue;
    if (c >= '0' && c <= '9') continue;
    if (c == '_' || c == '-') continue;
    // Explicitly reserving these
    if (c == '.' || c == '/' || c == '@') {
      return false;
    }
    return false;
  }
  return true;
}

uint8_t phenom_counter_scope_get_num_slots(
    phenom_counter_scope_t *scope)
{
  return scope->num_slots;
}

const char *phenom_counter_scope_get_name(
    phenom_counter_scope_t *scope)
{
  return scope->full_scope_name;
}

static void scope_id_hash(ck_ht_hash_t *hash, const void *key,
    size_t len, uint64_t seed)
{
  unused_parameter(len);
  unused_parameter(seed);
  hash->value = 1 + *(uint32_t*)key;
}

phenom_counter_scope_t *phenom_counter_scope_define(
    phenom_counter_scope_t *parent,
    const char *path,
    uint8_t max_counters)
{
  phenom_counter_scope_t *scope;
  int full_name_len = 0;
  int fail = 0;
  ck_ht_hash_t hash;
  struct ck_ht_entry entry;

  if (!is_valid_name(path)) {
    return NULL;
  }

  pthread_once(&done_tls_init, phenom_counter_init);

  if (parent) {
    full_name_len = strlen(path) + 1 /* '/' */ +
      strlen(parent->full_scope_name);
  }

  scope = calloc(1, sizeof(*scope) +
      // scope_name
      (1 + strlen(path)) +
      // space for full name if different (len + NUL byte)
      (full_name_len ? full_name_len + 1 : 0) +
      // slot_names
      ((max_counters - 1) * sizeof(char*)));
  if (!scope) {
    return NULL;
  }

  // Want to ensure that we never end up with a 0 scope id,
  // even if we roll; the CK hash routines don't support
  // a 0 key in DIRECT mode.
  do {
    scope->scope_id = ck_pr_faa_32(&next_scope_id, 1);
  } while (scope->scope_id == 0);
  // pre-compute the hash value
  scope_id_hash(&scope->hash, &scope->scope_id, sizeof(scope->scope_id), 0);

  // caller owns this ref
  scope->refcnt = 1;
  scope->num_slots = max_counters;
  scope->scope_name = (char*)&scope->slot_names[max_counters];
  strcpy(scope->scope_name, path);


  if (full_name_len == 0) {
    scope->full_scope_name = scope->scope_name;
    full_name_len = strlen(scope->scope_name);
  } else {
    // name lives right after scope->scope_name
    scope->full_scope_name = scope->scope_name + strlen(path) + 1;
    snprintf(scope->full_scope_name,
        full_name_len + 1, "%s/%s", parent->full_scope_name, path);
  }

  // Record in the map

  // compute hash
  ck_ht_hash(&hash, &scope_map, scope->full_scope_name,
      full_name_len);
  // prepare hash entry
  ck_ht_entry_set(&entry, hash, scope->full_scope_name,
      full_name_len, scope);

  ck_spinlock_lock(&scope_map_lock);
  {
    if (ck_ht_put_spmc(&scope_map, hash, &entry)) {
      // map owns a new ref
      phenom_refcnt_add(&scope->refcnt);
    } else {
      // already exists
      fail = 1;
    }
  }
  ck_spinlock_unlock(&scope_map_lock);

  if (fail) {
    phenom_counter_scope_delref(scope);
    scope = NULL;
  }

  return scope;
}

phenom_counter_scope_t *phenom_counter_scope_resolve(
    phenom_counter_scope_t *parent,
    const char *path)
{
  char *full_name = NULL;
  int len;
  ck_ht_hash_t hash;
  struct ck_ht_entry entry;
  phenom_counter_scope_t *scope = NULL;

  if (parent) {
    len = asprintf(&full_name, "%s/%s",
        parent->full_scope_name, path);
  } else {
    full_name = (char*)path;
    len = strlen(path);
  }
  ck_ht_hash(&hash, &scope_map, full_name, len);
  ck_ht_entry_key_set(&entry, full_name, len);
  if (ck_ht_get_spmc(&scope_map, hash, &entry)) {
    // Got it
    scope = (phenom_counter_scope_t*)entry.value;
    // Note: if we ever allow removing scopes from the map,
    // we will need to find a way to make adding this ref
    // race free wrt. that removal operation
    phenom_refcnt_add(&scope->refcnt);
  }

  if (full_name != path) {
    free(full_name);
  }

  return scope;
}

void phenom_counter_scope_delref(phenom_counter_scope_t *scope)
{
  if (!phenom_refcnt_del(&scope->refcnt)) {
    return;
  }

  free(scope);
}

uint8_t phenom_counter_scope_register_counter(
    phenom_counter_scope_t *scope,
    const char *name)
{
  uint8_t slot;

  do {
    if (scope->next_slot >= scope->num_slots) {
      return PHENOM_COUNTER_INVALID;
    }
    slot = scope->next_slot;
  } while (!ck_pr_cas_8(&scope->next_slot, slot, slot + 1));

  /* we've claimed slot; copy the name in */
  scope->slot_names[slot] = strdup(name);
  return slot;
}

bool phenom_counter_scope_register_counter_block(
    phenom_counter_scope_t *scope,
    uint8_t num_slots,
    uint8_t first_slot,
    const char **names)
{
  int i;

  if (scope->next_slot != first_slot) {
    return false;
  }
  if (scope->next_slot + num_slots > scope->num_slots) {
    return false;
  }
  scope->next_slot += num_slots;

  for (i = 0; i < num_slots; i++) {
    scope->slot_names[first_slot + i] = strdup(names[i]);
  }

  return true;
}

static struct phenom_counter_head *init_head(void)
{
  struct phenom_counter_head *head = calloc(1, sizeof(*head));

  if (!head) {
    return NULL;
  }

  if (!ck_ht_init(&head->ht, CK_HT_MODE_DIRECT, scope_id_hash,
        &ht_allocator, 32, 0)) {
    free(head);
    return NULL;
  }

  /* make it visible to stat consumers */
  ck_stack_push_upmc(&all_heads, &head->stack_entry);

  pthread_setspecific(tls_key, head);

  return head;
}

static phenom_counter_block_t *get_block_for_scope(
    phenom_counter_scope_t *scope)
{
  /* locate my counter head */
  struct phenom_counter_head *head;
  struct phenom_counter_block *block = NULL;
  struct ck_ht_entry entry;

  head = pthread_getspecific(tls_key);
  if (!head) {
    head = init_head();
    if (!head) {
      return NULL;
    }
  }

  /* locate my counter block */
  ck_ht_entry_key_set_direct(&entry, scope->scope_id);
  if (ck_ht_get_spmc(&head->ht, scope->hash, &entry)) {
    /* got it! */
    return (struct phenom_counter_block*)entry.value;
  }

  /* not present; we get to create it */
  block = calloc(1, sizeof(*block) +
      ((scope->num_slots - 1) * sizeof(int64_t)));
  if (!block) {
    return NULL;
  }
  /* the hash table owns this reference */
  block->refcnt = 1;
  ck_sequence_init(&block->seqlock);
  ck_ht_entry_set_direct(&entry, scope->hash,
      scope->scope_id, (uintptr_t)block);
  if (!ck_ht_put_spmc(&head->ht, scope->hash, &entry)) {
    free(block);
    return NULL;
  }

  return block;
}

void phenom_counter_scope_add(
    phenom_counter_scope_t *scope,
    uint8_t offset,
    int64_t value)
{
  phenom_counter_block_t *block = get_block_for_scope(scope);

  if (!block) return;

  /* now update the counter value */
  ck_sequence_write_begin(&block->seqlock);
  block->slots[offset] += value;
  ck_sequence_write_end(&block->seqlock);
}

phenom_counter_block_t *phenom_counter_block_open(
    phenom_counter_scope_t *scope)
{
  phenom_counter_block_t *block = get_block_for_scope(scope);

  if (!block) return NULL;

  phenom_refcnt_add(&block->refcnt);
  return block;
}

void phenom_counter_block_add(
    phenom_counter_block_t *block,
    uint8_t offset,
    int64_t value)
{
  ck_sequence_write_begin(&block->seqlock);
  block->slots[offset] += value;
  ck_sequence_write_end(&block->seqlock);
}

void phenom_counter_block_bulk_add(
    phenom_counter_block_t *block,
    uint8_t num_slots,
    const uint8_t *slots,
    const int64_t *values)
{
  int i;

  ck_sequence_write_begin(&block->seqlock);
  for (i = 0; i < num_slots; i++) {
    block->slots[slots[i]] += values[i];
  }
  ck_sequence_write_end(&block->seqlock);
}

void phenom_counter_block_delref(
    phenom_counter_block_t *block)
{
  if (!phenom_refcnt_del(&block->refcnt)) {
    return;
  }

  free(block);
}

int64_t phenom_counter_scope_get(
    phenom_counter_scope_t *scope,
    uint8_t offset)
{
  int64_t res = 0, val;
  ck_stack_entry_t *stack_entry;
  struct phenom_counter_head *head;
  struct phenom_counter_block *block;
  struct ck_ht_entry entry;
  unsigned int vers;

  CK_STACK_FOREACH(&all_heads, stack_entry) {
    head = phenom_counter_head_from_stack_entry(stack_entry);
    /* locate counter block */
    ck_ht_entry_key_set_direct(&entry, scope->scope_id);
    if (!ck_ht_get_spmc(&head->ht, scope->hash, &entry)) {
      continue;
    }

    block = (struct phenom_counter_block*)entry.value;
    do {
      vers = ck_sequence_read_begin(&block->seqlock);
      val = block->slots[offset];
    } while (ck_sequence_read_retry(&block->seqlock, vers));

    res += val;
  }

  return res;
}

uint8_t phenom_counter_scope_get_view(
    phenom_counter_scope_t *scope,
    uint8_t num_slots,
    int64_t *slots,
    const char **names)
{
  int i;
  ck_stack_entry_t *stack_entry;
  struct phenom_counter_head *head;
  struct phenom_counter_block *block;
  struct ck_ht_entry entry;
  unsigned int vers;
  int64_t *local_slots;

  if (num_slots > scope->num_slots) {
    num_slots = scope->num_slots;
  }
  if (num_slots > scope->next_slot) {
    num_slots = scope->next_slot;
  }

  local_slots = alloca(num_slots * sizeof(int64_t));
  memset(slots, 0, sizeof(*slots) * num_slots);

  CK_STACK_FOREACH(&all_heads, stack_entry) {
    head = phenom_counter_head_from_stack_entry(stack_entry);

    /* locate counter block */
    ck_ht_entry_key_set_direct(&entry, scope->scope_id);
    if (!ck_ht_get_spmc(&head->ht, scope->hash, &entry)) {
      continue;
    }

    block = (struct phenom_counter_block*)entry.value;
    do {
      vers = ck_sequence_read_begin(&block->seqlock);
      memcpy(local_slots, block->slots, num_slots * sizeof(int64_t));
    } while (ck_sequence_read_retry(&block->seqlock, vers));

    for (i = 0; i < num_slots; i++) {
      slots[i] += local_slots[i];
    }
  }

  if (names) {
    memcpy(names, scope->slot_names, num_slots * sizeof(char*));
  }

  return num_slots;
}

void phenom_counter_scope_iterator_init(
    phenom_counter_scope_iterator_t *iter)
{
  ck_ht_iterator_init((ck_ht_iterator_t*)iter);
}

phenom_counter_scope_t *phenom_counter_scope_iterator_next(
    phenom_counter_scope_iterator_t *iter)
{
  ck_ht_entry_t *entry;

  if (!ck_ht_next(&scope_map, (ck_ht_iterator_t*)iter,
        &entry)) {
    return NULL;
  }

  phenom_counter_scope_t *scope = (phenom_counter_scope_t*)entry->value;
  phenom_refcnt_add(&scope->refcnt);
  return scope;
}

/* vim:ts=2:sw=2:et:
 */

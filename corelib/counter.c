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
#include "phenom/sysutil.h"
#include "phenom/printf.h"
#include "phenom/queue.h"
#include <ck_epoch.h>
#include <ck_hs.h>
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

struct ph_counter_scope {
  ph_refcnt_t refcnt;
  uint32_t scope_id;
  unsigned long hash;

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
struct ph_counter_head {
  /* linkage so that a stat reader can find all blocks */
  ck_stack_entry_t stack_entry;
  /* maps counter_id to a block instance in this thread */
  ck_hs_t ht;
  /* for SMR */
  ck_epoch_record_t epoch_record;
};

/* This defines a function called ph_counter_head_from_stack_entry
 * that maps a ck_stack_entry_t to a ph_counter_head */
CK_STACK_CONTAINER(struct ph_counter_head,
    stack_entry, ph_counter_head_from_stack_entry)

/** map of fully qualified name to scope.
 * The CK hash set is single producer, multi-consumer,
 * meaning that we only need to serialize write operations
 * on the hash table; reads can proceed safely without a lock. */
static ck_hs_t scope_map;
static ck_spinlock_t scope_map_lock = CK_SPINLOCK_INITIALIZER;

/** key to TLS for finding counter block instances */
static pthread_key_t tls_key;
static ck_epoch_t ph_counter_epoch;

static pthread_once_t done_tls_init = PTHREAD_ONCE_INIT;

#ifdef HAVE___THREAD
static __thread struct ph_counter_head *myhead = NULL;
#endif

static CK_CC_INLINE struct ph_counter_head *get_counter_head(void)
{
#ifdef HAVE___THREAD
  return myhead;
#else
  return pthread_getspecific(tls_key);
#endif
}

static struct ph_counter_head *init_head(void);
static CK_CC_INLINE ck_epoch_record_t *get_epoch_record(void)
{
  struct ph_counter_head *head = get_counter_head();

  if (ph_unlikely(!head)) {
    head = init_head();
    if (!head) {
      return NULL;
    }
  }

  return &head->epoch_record;
}

static ck_stack_t all_heads = CK_STACK_INITIALIZER;

/** atomic scope identifier */
static uint32_t next_scope_id = 0;

static void ph_counter_head_tls_dtor(void *ptr)
{
  struct ph_counter_head *head = ptr;

  // Thread is exiting.  We don't care about reclaiming the memory
  // from around the scope maps as we're recycling the record anyway and if we
  // try something like a barrier here, we can deadlock all threads on
  // scheduler shutdown
  ck_epoch_unregister(&ph_counter_epoch, &head->epoch_record);
}

/** We need to allocate space for our epoch-based SMR to manage its stack
 * of outstanding allocations. Since that data is not managed by the caller
 * of the allocation, but by the SMR manager itself, we don't provide that
 * region to the caller.
 */
static void *hs_malloc(size_t size)
{
  ck_epoch_entry_t *e;

  e = malloc(sizeof (*e) + size);

  return e + 1;
}

static void ph_counter_epoch_free(ck_epoch_entry_t *e)
{

  free(e);
}

static void hs_free(void *p, size_t b, bool r)
{
  ck_epoch_entry_t *e = p;
  e--; /* See comment above hs_malloc */

  ph_unused_parameter(b);

  if (r == true) {
    /* Freeing requires same memory reclamation */
    ck_epoch_call(&ph_counter_epoch, get_epoch_record(),
        e, ph_counter_epoch_free);
  } else {
    free(e);
  }
}

static struct ck_malloc hs_allocator = {
  .malloc = hs_malloc,
  .free = hs_free
};

/* Tear things down and make valgrind happy that we didn't leak */
static void counter_destroy(void)
{
  ck_hs_iterator_t iter;
  ck_stack_entry_t *stack_entry;
  struct ph_counter_head *head;
  struct ph_counter_block *block;
  ph_counter_scope_t *scope;

  // Nuke all the heads and their associated epoch_records.
  // This is by definition unsafe but we're the last thread standing
  // at this point; we don't need to (and must not!) use the epoch
  // functions to reclaim this memory; just free with prejudice
  while ((stack_entry = ck_stack_pop_npsc(&all_heads)) != NULL) {
    ck_hs_iterator_t hiter;

    head = ph_counter_head_from_stack_entry(stack_entry);
    ck_hs_iterator_init(&hiter);
    while (ck_hs_next(&head->ht, &hiter, (void**)(void*)&block)) {
      free(block);
    }

    ck_hs_destroy(&head->ht);
    free(head);
  }

  ck_hs_iterator_init(&iter);
  while (ck_hs_next(&scope_map, &iter, (void**)(void*)&scope)) {
    int i;
    for (i = 0; i < scope->next_slot; i++) {
      free(scope->slot_names[i]);
    }
    free(scope);
  }

  ck_hs_destroy(&scope_map);
}

static bool scope_map_compare(const void *a, const void *b)
{
  const ph_counter_scope_t *sa = a, *sb = b;
  size_t lena, lenb;

  lena = strlen(sa->full_scope_name);
  lenb = strlen(sb->full_scope_name);

  if (lena != lenb) {
    return 0;
  }

  // !!! guarantees correct boolean representation where 0 is the truth value
  return !!!memcmp(sa->full_scope_name, sb->full_scope_name, lena);
}

static unsigned long scope_map_hash(const void *key,
    unsigned long seed)
{
  const ph_counter_scope_t *sk = key;
  uint64_t h[2];

  ph_hash_bytes_murmur(sk->full_scope_name, strlen(sk->full_scope_name),
      seed, h);
  return h[0];
}

static void ph_counter_init(void)
{
  ck_epoch_init(&ph_counter_epoch);
  pthread_key_create(&tls_key, ph_counter_head_tls_dtor);

  if (sizeof(struct ph_counter_scope_iterator) !=
      sizeof(struct ck_hs_iterator)) {
    /* ideally, we'd let the compiler catch this, but we
     * don't want to pollute ph_counter.h with the CK
     * functions, because C++ compilers hate it.
     * If you're seeing this abort trigger, you need to
     * update struct ph_counter_scope_iterator to
     * have the same size as struct ck_hs_iterator */
    abort();
  }

  if (!ck_hs_init(&scope_map, CK_HS_MODE_SPMC | CK_HS_MODE_OBJECT,
      scope_map_hash, scope_map_compare, &hs_allocator, 65536, lrand48())) {
    // If this fails, we're screwed
    abort();
  }

  atexit(counter_destroy);
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

uint8_t ph_counter_scope_get_num_slots(
    ph_counter_scope_t *scope)
{
  return scope->num_slots;
}

const char *ph_counter_scope_get_name(
    ph_counter_scope_t *scope)
{
  return scope->full_scope_name;
}

static bool scope_id_compare(const void *a, const void *b)
{
  uint32_t A = *(uint32_t*)a;
  uint32_t B = *(uint32_t*)b;
  return A == B;
}

static unsigned long scope_id_hash(const void *key,
    unsigned long seed)
{
  ph_unused_parameter(seed);

  return 1 + *(uint32_t*)key;
}

ph_counter_scope_t *ph_counter_scope_define(
    ph_counter_scope_t *parent,
    const char *path,
    uint8_t max_counters)
{
  ph_counter_scope_t *scope;
  uint64_t hash = 0;
  int full_name_len = 0;
  int fail = 0;

  if (!is_valid_name(path)) {
    return NULL;
  }

  pthread_once(&done_tls_init, ph_counter_init);

  if (parent) {
    full_name_len = strlen(path) + 1 /* '.' */ +
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
  scope->hash = scope_id_hash(&scope->scope_id, 0);

  // caller owns this ref
  scope->refcnt = 1;
  scope->num_slots = max_counters;
  scope->scope_name = (char*)&scope->slot_names[max_counters];
  strcpy(scope->scope_name, path);


  if (full_name_len == 0) {
    scope->full_scope_name = scope->scope_name;
  } else {
    // name lives right after scope->scope_name
    scope->full_scope_name = scope->scope_name + strlen(path) + 1;
    snprintf(scope->full_scope_name,
        full_name_len + 1, "%s.%s", parent->full_scope_name, path);
  }

  // Record in the map

  // compute hash
  hash = CK_HS_HASH(&scope_map, scope_map_hash, scope);
  ck_spinlock_lock(&scope_map_lock);
  {
    ck_epoch_record_t *er = get_epoch_record();

    ck_epoch_begin(&ph_counter_epoch, er);
    if (ck_hs_get(&scope_map, hash, scope) != NULL) {
      int i;

      ck_epoch_end(&ph_counter_epoch, er);
      ck_spinlock_unlock(&scope_map_lock);

      for (i = 0; i < scope->next_slot; i++) {
        free(scope->slot_names[i]);
      }
      free(scope);

      return NULL;
    }

    if (ck_hs_put(&scope_map, hash, scope)) {
      // map owns a new ref
      ph_refcnt_add(&scope->refcnt);
    } else {
      fail = 1;
    }
    ck_epoch_end(&ph_counter_epoch, er);
  }
  ck_spinlock_unlock(&scope_map_lock);

  if (fail) {
    ph_counter_scope_delref(scope);
    scope = NULL;
  }

  return scope;
}

ph_counter_scope_t *ph_counter_scope_resolve(
    ph_counter_scope_t *parent,
    const char *path)
{
  char *full_name = NULL;
  uint64_t hash = 0;
  ph_counter_scope_t *scope = NULL;
  ph_counter_scope_t search;
  ck_epoch_record_t *er = get_epoch_record();

  if (parent) {
    ph_asprintf(&full_name, "%s.%s",
        parent->full_scope_name, path);
  } else {
    full_name = (char*)path;
  }

  search.full_scope_name = full_name;
  hash = CK_HS_HASH(&scope_map, scope_map_hash, &search);
  ck_epoch_begin(&ph_counter_epoch, er);
  scope = ck_hs_get(&scope_map, hash, &search);
  if (scope != NULL) {
    // Note: if we ever allow removing scopes from the map,
    // we will need to find a way to make adding this ref
    // race free wrt. that removal operation
    ph_refcnt_add(&scope->refcnt);
  }
  ck_epoch_end(&ph_counter_epoch, er);

  if (full_name != path) {
    free(full_name);
  }

  return scope;
}

void ph_counter_scope_delref(ph_counter_scope_t *scope)
{
  ck_epoch_record_t *er = get_epoch_record();
  int i;

  if (!ph_refcnt_del(&scope->refcnt)) {
    return;
  }

  for (i = 0; i < scope->next_slot; i++) {
    free(scope->slot_names[i]);
  }

  ck_epoch_begin(&ph_counter_epoch, er);
  ck_spinlock_lock(&scope_map_lock);
  {
    uint64_t hash;

    hash = CK_HS_HASH(&scope_map, scope_map_hash, scope);
    ck_hs_remove(&scope_map, hash, scope);
    free(scope);
  }
  ck_spinlock_unlock(&scope_map_lock);
  ck_epoch_end(&ph_counter_epoch, er);
  ck_epoch_barrier(&ph_counter_epoch, er);
}

uint8_t ph_counter_scope_register_counter(
    ph_counter_scope_t *scope,
    const char *name)
{
  uint8_t slot;

  do {
    if (scope->next_slot >= scope->num_slots) {
      return PH_COUNTER_INVALID;
    }
    slot = scope->next_slot;
  } while (!ck_pr_cas_8(&scope->next_slot, slot, slot + 1));

  /* we've claimed slot; copy the name in */
  scope->slot_names[slot] = strdup(name);
  return slot;
}

bool ph_counter_scope_register_counter_block(
    ph_counter_scope_t *scope,
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

static struct ph_counter_head *init_head(void)
{
  struct ph_counter_head *head;
  ck_epoch_record_t *er;

  // Try to recycle a head from a dead thread
  er = ck_epoch_recycle(&ph_counter_epoch);
  if (er) {
    head = (struct ph_counter_head*)
           ((char*)er - ph_offsetof(struct ph_counter_head, epoch_record));
  } else {
    // Otherwise, make a new one and prep it
    head = calloc(1, sizeof(*head));

    if (!head) {
      return NULL;
    }

    if (!ck_hs_init(&head->ht, CK_HS_MODE_SPMC|CK_HS_MODE_OBJECT,
        scope_id_hash, scope_id_compare, &hs_allocator, 32, 0)) {
      free(head);
      return NULL;
    }

    ck_epoch_register(&ph_counter_epoch, &head->epoch_record);

    /* make it visible to stat consumers */
    ck_stack_push_upmc(&all_heads, &head->stack_entry);
  }

  pthread_setspecific(tls_key, head);
#ifdef HAVE___THREAD
  myhead = head;
#endif

  return head;
}

static ph_counter_block_t *get_block_for_scope(
    ph_counter_scope_t *scope)
{
  /* locate my counter head */
  struct ph_counter_head *head;
  struct ph_counter_block *block = NULL;

  head = get_counter_head();

  if (ph_unlikely(!head)) {
    head = init_head();
    if (!head) {
      return NULL;
    }
  }

  /* locate my counter block */
  block = ck_hs_get(&head->ht, scope->hash, &scope->scope_id);
  if (ph_likely(block)) {
    /* got it! */
    return block;
  }

  /* not present; we get to create it */
  block = calloc(1, sizeof(*block) +
      ((scope->num_slots - 1) * sizeof(int64_t)));
  if (ph_unlikely(!block)) {
    return NULL;
  }
  /* the hash table owns this reference */
  block->refcnt = 1;
  block->scope_id = scope->scope_id;

  if (ph_unlikely(!ck_hs_put(&head->ht, scope->hash, &block->scope_id))) {
    free(block);
    return NULL;
  }

  return block;
}

void ph_counter_scope_add(
    ph_counter_scope_t *scope,
    uint8_t offset,
    int64_t value)
{
  ph_counter_block_t *block = get_block_for_scope(scope);

  if (ph_unlikely(!block)) return;
  ph_counter_block_add(block, offset, value);
}

ph_counter_block_t *ph_counter_block_open(
    ph_counter_scope_t *scope)
{
  ph_counter_block_t *block = get_block_for_scope(scope);

  if (!block) return NULL;

  ph_refcnt_add(&block->refcnt);
  return block;
}

void ph_counter_block_delref(
    ph_counter_block_t *block)
{
  if (!ph_refcnt_del(&block->refcnt)) {
    return;
  }

  free(block);
}

static inline uint32_t read_begin(ph_counter_block_t *block)
{
  uint32_t ver;

  for (;;) {
    ver = ck_pr_load_32(&block->seqno);

    if ((ver & 1) == 0) {
      ck_pr_fence_load();
      return ver;
    }

    ck_pr_stall();
  }
}

static inline bool read_retry(ph_counter_block_t *block, uint32_t vers)
{
  ck_pr_fence_load();
  return ck_pr_load_32(&block->seqno) != vers;
}

int64_t ph_counter_scope_get(
    ph_counter_scope_t *scope,
    uint8_t offset)
{
  int64_t res = 0, val;
  ck_stack_entry_t *stack_entry;
  struct ph_counter_head *head;
  struct ph_counter_block *block;
  unsigned int vers;

  CK_STACK_FOREACH(&all_heads, stack_entry) {
    head = ph_counter_head_from_stack_entry(stack_entry);
    /* locate counter block */
    block = ck_hs_get(&head->ht, scope->hash, &scope->scope_id);
    if (!block) {
      continue;
    }

    do {
      vers = read_begin(block);
      val = block->slots[offset];
    } while (read_retry(block, vers));

    res += val;
  }

  return res;
}

uint8_t ph_counter_scope_get_view(
    ph_counter_scope_t *scope,
    uint8_t num_slots,
    int64_t *slots,
    const char **names)
{
  int i;
  ck_stack_entry_t *stack_entry;
  struct ph_counter_head *head;
  struct ph_counter_block *block;
  unsigned int vers;
  int64_t *local_slots;
  ck_epoch_record_t *er = get_epoch_record();

  if (num_slots > scope->num_slots) {
    num_slots = scope->num_slots;
  }
  if (num_slots > scope->next_slot) {
    num_slots = scope->next_slot;
  }

  local_slots = alloca(num_slots * sizeof(int64_t));
  memset(slots, 0, sizeof(*slots) * num_slots);

  ck_epoch_begin(&ph_counter_epoch, er);
  CK_STACK_FOREACH(&all_heads, stack_entry) {
    head = ph_counter_head_from_stack_entry(stack_entry);

    /* locate counter block */
    block = ck_hs_get(&head->ht, scope->hash, &scope->scope_id);
    if (!block) {
      continue;
    }

    do {
      vers = read_begin(block);
      memcpy(local_slots, block->slots, num_slots * sizeof(int64_t));
    } while (read_retry(block, vers));

    for (i = 0; i < num_slots; i++) {
      slots[i] += local_slots[i];
    }
  }
  ck_epoch_end(&ph_counter_epoch, er);

  if (names) {
    memcpy(names, scope->slot_names, num_slots * sizeof(char*));
  }

  return num_slots;
}

void ph_counter_scope_iterator_init(
    ph_counter_scope_iterator_t *iter)
{
  ck_hs_iterator_init((ck_hs_iterator_t*)iter);
}

ph_counter_scope_t *ph_counter_scope_iterator_next(
    ph_counter_scope_iterator_t *iter)
{
  ck_epoch_record_t *er = get_epoch_record();
  ph_counter_scope_t *scope;
  void *i;

  ck_epoch_begin(&ph_counter_epoch, er);
  if (!ck_hs_next(&scope_map, (ck_hs_iterator_t*)iter, &i)) {
    ck_epoch_end(&ph_counter_epoch, er);
    return NULL;
  }
  ck_epoch_end(&ph_counter_epoch, er);

  scope = i;
  ph_refcnt_add(&scope->refcnt);
  return scope;
}

/* vim:ts=2:sw=2:et:
 */

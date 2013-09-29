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

#include <phenom/defs.h>
#include "phenom/hashtable.h"
#include "phenom/sysutil.h"
#include "phenom/string.h"
#include "phenom/memory.h"
#include "phenom/log.h"

static ph_memtype_t mt_table;
static struct ph_memtype_def table_def = {
  "hashtable", "table", 0, PH_MEM_FLAGS_ZERO
};

static void init_hashtable(void)
{
  mt_table = ph_memtype_register(&table_def);
}

PH_LIBRARY_INIT_PRI(init_hashtable, 0, 5)

static inline void *keyptr(struct ph_ht_elem *elem)
{
  return (char*)elem + sizeof(elem->status);
}

static inline void *valptr(ph_ht_t *ht, struct ph_ht_elem *elem)
{
  return (char*)elem + sizeof(elem->status) + ht->kdef->ksize;
}

static inline struct ph_ht_elem *elemptr(ph_ht_t *ht, uint32_t p)
{
  return (struct ph_ht_elem*)(void*)(ht->table + (p * ht->elem_size));
}

ph_result_t ph_ht_init(ph_ht_t *ht, uint32_t size_hint,
    const struct ph_ht_key_def *kdef,
    const struct ph_ht_val_def *vdef)
{
  ht->kdef = kdef;
  ht->vdef = vdef;
  ht->nelems = 0;
  ht->table_size = ph_power_2(size_hint * 2);
  ht->elem_size = sizeof(struct ph_ht_elem) + kdef->ksize + vdef->vsize;
  ht->mask = ht->table_size - 1;
  ht->table = ph_mem_alloc_size(mt_table, ht->elem_size * ht->table_size);
  if (!ht->table) {
    return PH_NOMEM;
  }

  return PH_OK;
}

void ph_ht_destroy(ph_ht_t *ht)
{
  ph_ht_free_entries(ht);
  ph_mem_free(mt_table, ht->table);
  ht->table = 0;
}

static inline int key_compare(ph_ht_t *ht, const void *a, const void *b)
{
  if (ht->kdef->key_compare) {
    return ht->kdef->key_compare(a, b);
  }
  return memcmp(a, b, ht->kdef->ksize);
}

static inline bool key_copy(ph_ht_t *ht, const void *src,
    void *dest, int flags)
{
  if (ht->kdef->key_copy && (flags & PH_HT_CLAIM_KEY) == 0) {
    return ht->kdef->key_copy(src, dest);
  }
  memcpy(dest, src, ht->kdef->ksize);
  return true;
}

static inline bool val_copy(ph_ht_t *ht, const void *src,
    void *dest, int flags)
{
  if (ht->vdef->val_copy && (flags & PH_HT_CLAIM_VAL) == 0) {
    return ht->vdef->val_copy(src, dest);
  }
  memcpy(dest, src, ht->vdef->vsize);
  return true;
}

static inline void val_delete(ph_ht_t *ht, void *val)
{
  if (ht->vdef->val_delete) {
    ht->vdef->val_delete(val);
  }
}

static inline void key_delete(ph_ht_t *ht, void *key)
{
  if (ht->kdef->key_delete) {
    ht->kdef->key_delete(key);
  }
}

void ph_ht_free_entries(ph_ht_t *ht)
{
  uint32_t i;
  struct ph_ht_elem *elem;

  for (i = 0; i < ht->table_size; i++) {
    elem = (struct ph_ht_elem*)(void*)(ht->table + (i * ht->elem_size));

    if (elem->status == PH_HT_ELEM_EMPTY) {
      continue;
    }
    if (elem->status == PH_HT_ELEM_TOMBSTONE) {
      elem->status = PH_HT_ELEM_EMPTY;
      continue;
    }

    key_delete(ht, keyptr(elem));
    val_delete(ht, valptr(ht, elem));

    elem->status = PH_HT_ELEM_EMPTY;
  }
  ht->nelems = 0;
}

static struct ph_ht_elem *find_elem_slot(ph_ht_t *ht, const void *key)
{
  uint32_t pos = ht->kdef->hash_func(key) & ht->mask;
  struct ph_ht_elem *elem, *tomb = NULL;
  uint32_t attempts = 0;

  while (attempts++ < ht->table_size) {
    elem = elemptr(ht, pos & ht->mask);

    if (elem->status == PH_HT_ELEM_TOMBSTONE) {
      // Remember the first tombstone
      if (!tomb) {
        tomb = elem;
      }
      pos++;
      continue;
    }

    if (elem->status == PH_HT_ELEM_EMPTY) {
      // Element is not in the table; reuse a tombstone
      // if we found one, otherwise claim this empty elem
      return tomb ? tomb : elem;
    }

    if (key_compare(ht, key, keyptr(elem)) == 0) {
      return elem;
    }

    // Not this one, look for another
    pos++;
  }

  // There were no empty slots in the whole table, we need to rebuild
  return NULL;
}

static struct ph_ht_elem *find_new_slot(ph_ht_t *ht, char *table,
    uint32_t mask, const void *key)
{
  uint32_t pos = ht->kdef->hash_func(key) & mask;
  struct ph_ht_elem *elem;

  while (true) {
    elem = (struct ph_ht_elem*)(void*)(table + ((pos & mask) * ht->elem_size));

    if (elem->status == PH_HT_ELEM_EMPTY) {
      return elem;
    }

    pos++;
  }
}

static bool rebuild_table(ph_ht_t *ht, uint32_t size)
{
  char *table;
  uint32_t mask, i, done;

  mask = size - 1;

  table = ph_mem_alloc_size(mt_table, ht->elem_size * size);
  if (!table) {
    return false;
  }

  for (i = 0, done = 0; done < ht->nelems && i < ht->table_size; i++) {
    struct ph_ht_elem *src, *dest;

    src = elemptr(ht, i);
    if (src->status == PH_HT_ELEM_TAKEN) {
      dest = find_new_slot(ht, table, mask, keyptr(src));

      memcpy(dest, src, ht->elem_size);
      done++;
    }
  }

  ph_mem_free(mt_table, ht->table);
  ht->table = table;
  ht->table_size = size;
  ht->mask = mask;
  return true;
}

bool ph_ht_grow(ph_ht_t *ht, uint32_t nelems)
{
  uint32_t size = ph_power_2(nelems * 2);

  if (size <= ht->table_size) {
    return true;
  }

  return rebuild_table(ht, size);
}

ph_result_t ph_ht_insert(ph_ht_t *ht, void *key, void *value, int flags)
{
  struct ph_ht_elem *elem;

  elem = find_elem_slot(ht, key);
  if (!elem || (elem->status != PH_HT_ELEM_TAKEN &&
      ht->nelems + 1 > ht->table_size >> 1)) {
    // Getting full
    if (rebuild_table(ht, ht->table_size << 1)) {
      elem = find_elem_slot(ht, key);
    }
    if (!elem) {
      return PH_NOMEM;
    }
  }

  if (elem->status == PH_HT_ELEM_TAKEN) {
    if ((flags & PH_HT_REPLACE) == 0) {
      return PH_EXISTS;
    }

    val_delete(ht, valptr(ht, elem));
    if (!val_copy(ht, value, valptr(ht, elem), flags)) {
      elem->status = PH_HT_ELEM_TOMBSTONE;
      ht->nelems--;
      return PH_ERR;
    }

    // If they wanted us to claim the key, we don't need it here
    // but they're expecting not to worry about referencing it
    // anymore, so we delete it
    if (flags & PH_HT_CLAIM_KEY) {
      key_delete(ht, key);
    }

    elem->status = PH_HT_ELEM_TAKEN;
    return PH_OK;
  }

  if (!key_copy(ht, key, keyptr(elem), flags)) {
    return PH_ERR;
  }
  if (!val_copy(ht, value, valptr(ht, elem), flags)) {
    // Undo the key copy
    if (flags & PH_HT_COPY_KEY) {
      key_delete(ht, keyptr(elem));
    }
    return PH_ERR;
  }
  elem->status = PH_HT_ELEM_TAKEN;
  ht->nelems++;
  return PH_OK;
}

ph_result_t ph_ht_set(ph_ht_t *ht, void *key, void *value)
{
  return ph_ht_insert(ht, key, value, PH_HT_NO_REPLACE|PH_HT_COPY);
}

ph_result_t ph_ht_replace(ph_ht_t *ht, void *key, void *value)
{
  return ph_ht_insert(ht, key, value, PH_HT_REPLACE|PH_HT_COPY);
}

void *ph_ht_get(ph_ht_t *ht, const void *key)
{
  struct ph_ht_elem *elem;

  elem = find_elem_slot(ht, key);
  if (!elem || elem->status != PH_HT_ELEM_TAKEN) {
    return 0;
  }
  return valptr(ht, elem);
}

ph_result_t ph_ht_lookup(ph_ht_t *ht, const void *key, void *val, bool copy)
{
  struct ph_ht_elem *elem;

  elem = find_elem_slot(ht, key);
  if (!elem || elem->status != PH_HT_ELEM_TAKEN) {
    return PH_NOENT;
  }
  if (!val_copy(ht, valptr(ht, elem), val,
        copy ? PH_HT_COPY_VAL : PH_HT_CLAIM_VAL)) {
    return PH_ERR;
  }
  return PH_OK;
}

ph_result_t ph_ht_del(ph_ht_t *ht, const void *key)
{
  struct ph_ht_elem *elem;

  elem = find_elem_slot(ht, key);
  if (!elem || elem->status != PH_HT_ELEM_TAKEN) {
    return PH_NOENT;
  }

  key_delete(ht, keyptr(elem));
  val_delete(ht, valptr(ht, elem));

  elem->status = PH_HT_ELEM_TOMBSTONE;
  ht->nelems--;

  return PH_OK;
}

uint32_t ph_ht_size(ph_ht_t *ht)
{
  return ht->nelems;
}

bool ph_ht_iter_next(ph_ht_t *ht, ph_ht_iter_t *iter, void **key, void **val)
{
  struct ph_ht_elem *elem;

  if (iter->size != ht->table_size) {
    return false;
  }

  while (iter->slot < ht->table_size) {
    elem = elemptr(ht, iter->slot);
    iter->slot++;

    if (elem->status == PH_HT_ELEM_TAKEN) {
      if (key) {
        *key = keyptr(elem);
      }
      if (val) {
        *val = valptr(ht, elem);
      }
      return true;
    }
  }
  return false;
}

bool ph_ht_iter_first(ph_ht_t *ht, ph_ht_iter_t *iter, void **key, void **val)
{
  iter->slot = 0;
  iter->size = ht->table_size;
  return ph_ht_iter_next(ht, iter, key, val);
}

bool ph_ht_ordered_iter_first(ph_ht_t *ht, ph_ht_ordered_iter_t *iter,
    void **key, void **val)
{
  uint32_t i;
  char *kptr;
  struct ph_ht_elem *elem;

  if (ht->nelems == 0) {
    iter->size = 0;
    iter->slot = 1;
    iter->slots = 0;
    return false;
  }

  if (ph_unlikely(ht->kdef->key_compare == NULL)) {
    ph_panic("you must define a key_compare function to use ordered_iter");
  }

  iter->size = ht->nelems;
  iter->slot = 1;
  iter->slots = ph_mem_alloc_size(mt_table, ht->nelems * ht->kdef->ksize);

  if (!iter->slots) {
    return false;
  }

  // Collect the items in their physical order
  kptr = iter->slots;
  for (i = 0; i < ht->table_size; i++) {
    elem = elemptr(ht, i);

    if (elem->status != PH_HT_ELEM_TAKEN) {
      continue;
    }

    key_copy(ht, keyptr(elem), kptr, PH_HT_COPY_KEY);
    kptr += ht->kdef->ksize;
  }

  qsort(iter->slots, ht->nelems, ht->kdef->ksize, ht->kdef->key_compare);

  elem = find_elem_slot(ht, iter->slots);
  if (!elem || elem->status != PH_HT_ELEM_TAKEN) {
    // Shouldn't happen
    ph_ht_ordered_iter_end(ht, iter);
    return false;
  }

  if (key) {
    *key = keyptr(elem);
  }
  if (val) {
    *val = valptr(ht, elem);
  }

  return true;
}

bool ph_ht_ordered_iter_next(ph_ht_t *ht, ph_ht_ordered_iter_t *iter,
    void **key, void **val)
{
  char *kptr;
  struct ph_ht_elem *elem;

  if (iter->slot >= iter->size || iter->slots == 0) {
    return false;
  }

  kptr = iter->slots + (ht->kdef->ksize * iter->slot);
  elem = find_elem_slot(ht, kptr);
  if (!elem || elem->status != PH_HT_ELEM_TAKEN) {
    // Shouldn't happen
    return false;
  }

  if (key) {
    *key = keyptr(elem);
  }
  if (val) {
    *val = valptr(ht, elem);
  }

  iter->slot++;
  return true;
}

void ph_ht_ordered_iter_end(ph_ht_t *ht, ph_ht_ordered_iter_t *iter)
{
  uint32_t i;

  if (!iter->slots) {
    return;
  }

  for (i = 0; i < iter->size; i++) {
    key_delete(ht, iter->slots + (i * ht->kdef->ksize));
  }

  ph_mem_free(mt_table, iter->slots);
  iter->slots = 0;
}

static uint32_t string_hash(const void *key)
{
  uint64_t hval[2];
  ph_string_t *str = *(ph_string_t**)key;

  ph_hash_bytes_murmur(str->buf, str->len, 0, hval);
  return (uint32_t)hval[0];
}

static int string_compare(const void *a, const void *b)
{
  const ph_string_t *astr = *(ph_string_t**)a;
  const ph_string_t *bstr = *(ph_string_t**)b;

  return ph_string_compare(astr, bstr);
}

static bool string_copy(const void *src, void *dest)
{
  ph_string_t *astr = *(ph_string_t**)src;
  ph_string_t **bstr = (ph_string_t**)dest;

  *bstr = astr;
  ph_string_addref(astr);

  return true;
}

static void string_delete(void *key)
{
  ph_string_t **strp = (ph_string_t**)key;

  if (*strp) {
    ph_string_delref(*strp);
    *strp = 0;
  }
}

struct ph_ht_key_def ph_ht_string_key_def = {
  sizeof(ph_string_t*),
  string_hash,
  string_compare,
  string_copy,
  string_delete
};

struct ph_ht_val_def ph_ht_string_val_def = {
  sizeof(ph_string_t*),
  string_copy,
  string_delete
};

struct ph_ht_val_def ph_ht_ptr_val_def = {
  sizeof(void*),
  NULL,
  NULL
};


/* vim:ts=2:sw=2:et:
 */

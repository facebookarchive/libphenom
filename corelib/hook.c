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

#include "phenom/defs.h"
#include "phenom/thread.h"
#include "phenom/sysutil.h"
#include "phenom/hook.h"
#include "phenom/hashtable.h"
#include <ck_rwlock.h>

struct ph_hook_item_free {
  ck_epoch_entry_t entry;
  void *closure;
  ph_hook_func func;
  ph_hook_unreg_func unreg;
};

static ph_ht_t hook_hash;
static ck_rwlock_t rwlock = CK_RWLOCK_INITIALIZER;
static ph_memtype_def_t defs[] = {
  { "hook", "hook", sizeof(ph_hook_point_t), PH_MEM_FLAGS_ZERO },
  { "hook", "head", 0, 0 },
  { "hook", "string", 0, 0 },
  { "hook", "unreg", sizeof(struct ph_hook_item_free), 0 },
};
static struct {
  ph_memtype_t hookpoint, head, string, unreg;
} mt;

static void hookpoint_delete(void *val)
{
  ph_hook_point_t *hook = *(ph_hook_point_t**)val;

  if (hook->head) {
    ph_mem_free(mt.head, hook->head);
  }
  ph_mem_free(mt.hookpoint, hook);
}

static struct ph_ht_val_def hookpoint_def = {
  sizeof(void*),
  0,
  hookpoint_delete
};

static void do_hook_fini(void)
{
  ph_ht_destroy(&hook_hash);
}

static void do_hook_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.hookpoint);
  ph_ht_init(&hook_hash, 32, &ph_ht_string_key_def, &hookpoint_def);
}

PH_LIBRARY_INIT_PRI(do_hook_init, do_hook_fini, 6)

ph_hook_point_t *ph_hook_point_get(ph_string_t *name, bool create)
{
  ph_hook_point_t *hp = 0;

  ck_rwlock_read_lock(&rwlock);
  {
    ph_ht_lookup(&hook_hash, &name, &hp, false);
  }
  ck_rwlock_read_unlock(&rwlock);

  if (hp || !create) {
    return hp;
  }

  ck_rwlock_write_lock(&rwlock);
  {
    // Look again: someone may have populated while we were unlocked
    ph_ht_lookup(&hook_hash, &name, &hp, false);
    if (!hp) {
      hp = ph_mem_alloc(mt.hookpoint);
      if (hp) {
        if (ph_ht_set(&hook_hash, &name, &hp) != PH_OK) {
          ph_mem_free(mt.hookpoint, hp);
          hp = NULL;
        }
      }
    }
  }
  ck_rwlock_write_unlock(&rwlock);

  return hp;
}

ph_hook_point_t *ph_hook_point_get_cstr(const char *name, bool create)
{
  ph_string_t sstr, *str;
  ph_hook_point_t *hp;

  if (create) {
    str = ph_string_make_cstr(mt.string, name);
  } else {
    uint32_t len = strlen(name);
    ph_string_init_claim(&sstr, PH_STRING_STATIC, (char*)name, len, len);
    str = &sstr;
  }

  hp = ph_hook_point_get(str, create);

  if (str != &sstr) {
    ph_string_delref(str);
  }

  return hp;
}

static int compare_item(const void *a, const void *b)
{
  const ph_hook_item_t *A = a;
  const ph_hook_item_t *B = b;
  ptrdiff_t diff = A->pri - B->pri;
  union {
    uint8_t *p;
    ph_hook_func f;
  } pa, pb;

  if (diff != 0) {
    return (int)diff;
  }

  pa.f = A->func;
  pb.f = B->func;
  diff = pa.p - pb.p;
  if (diff != 0) {
    return (int)diff;
  }

  diff = (char*)A->closure - (char*)B->closure;
  return (int)diff;
}

static void free_head(ck_epoch_entry_t *ent)
{
  ph_static_assert(ph_offsetof(ph_hook_point_head_t, entry) == 0,
      entry_must_be_first);

  ph_mem_free(mt.head, ent);
}

static ph_result_t do_register(ph_hook_point_t *hp, ph_hook_func func,
    void *closure, int8_t pri, ph_hook_unreg_func unreg)
{
  ph_hook_point_head_t *old_head, *new_head;
  ph_result_t res = PH_ERR;
  uint16_t num_items = 0;

  ck_rwlock_write_lock(&rwlock);
  {
    old_head = hp->head;

    if (old_head) {
      num_items = old_head->nitems;
    }

    // num_items is 1-less than the number we want, but the head struct
    // has 1 element embedded, so we're fine to multiply it out here
    new_head = ph_mem_alloc_size(mt.head,
        sizeof(*new_head) + (sizeof(ph_hook_item_t) * (num_items)));

    if (!new_head) {
      goto done;
    }

    new_head->nitems = num_items + 1;

    // Copy old data in
    if (old_head) {
      memcpy(new_head->items, old_head->items,
          old_head->nitems * sizeof(ph_hook_item_t));
    }

    new_head->items[num_items].closure = closure;
    new_head->items[num_items].func = func;
    new_head->items[num_items].pri = pri;
    new_head->items[num_items].unreg = unreg;

    qsort(new_head->items, new_head->nitems,
        sizeof(ph_hook_item_t), compare_item);

    hp->head = new_head;

    if (old_head) {
      ph_thread_epoch_defer(&old_head->entry, free_head);
    }

    res = PH_OK;
  }
done:
  ck_rwlock_write_unlock(&rwlock);

  return res;
}

static void call_unreg(ck_epoch_entry_t *ent)
{
  struct ph_hook_item_free *unreg;

  ph_static_assert(ph_offsetof(struct ph_hook_item_free, entry) == 0,
      entry_must_be_first);

  unreg = (struct ph_hook_item_free*)ent;

  unreg->unreg(unreg->closure, unreg->func);

  ph_mem_free(mt.unreg, unreg);
}

static ph_result_t do_unregister(ph_hook_point_t *hp, ph_hook_func func,
    void *closure)
{
  ph_hook_point_head_t *old_head, *new_head;
  ph_result_t res = PH_ERR;
  uint16_t off = 0;
  bool found = false;
  struct ph_hook_item_free *unreg = 0;

  ck_rwlock_write_lock(&rwlock);
  {
    old_head = hp->head;

    if (!old_head) {
      goto done;
    }

    for (off = 0; off < old_head->nitems; off++) {
      if (old_head->items[off].func == func &&
          old_head->items[off].closure == closure) {
        found = true;
        break;
      }
    }

    if (!found) {
      goto done;
    }

    new_head = ph_mem_alloc_size(mt.head,
        sizeof(*new_head) + (sizeof(ph_hook_item_t) * (old_head->nitems-1)));

    if (!new_head) {
      goto done;
    }

    if (old_head->items[off].unreg) {
      unreg = ph_mem_alloc(mt.unreg);
      if (!unreg) {
        ph_mem_free(mt.head, new_head);
        goto done;
      }

      unreg->closure = old_head->items[off].closure;
      unreg->func = old_head->items[off].func;
      unreg->unreg = old_head->items[off].unreg;
    }

    new_head->nitems = old_head->nitems - 1;

    // Copy before the item
    if (off) {
      memcpy(new_head->items, old_head->items, off * sizeof(ph_hook_item_t));
    }
    // Copy after the item
    if (off + 1 <= old_head->nitems) {
      memcpy(new_head->items + off, old_head->items + off + 1,
          (old_head->nitems - (off+1)) * sizeof(ph_hook_item_t));
    }

    // Don't need to re-sort, since we simply removed that item

    hp->head = new_head;
    ph_thread_epoch_defer(&old_head->entry, free_head);

    // Arrange to unregister
    if (unreg) {
      ph_thread_epoch_defer(&unreg->entry, call_unreg);
    }

    res = PH_OK;
  }
done:
  ck_rwlock_write_unlock(&rwlock);

  return res;
}

ph_result_t ph_hook_register(ph_string_t *name, ph_hook_func func,
    void *closure, int8_t pri, ph_hook_unreg_func unreg)
{
  ph_hook_point_t *hp = ph_hook_point_get(name, true);

  if (!hp) {
    return PH_ERR;
  }

  return do_register(hp, func, closure, pri, unreg);
}

ph_result_t ph_hook_register_cstr(const char *name, ph_hook_func func,
    void *closure, int8_t pri, ph_hook_unreg_func unreg)
{
  ph_hook_point_t *hp = ph_hook_point_get_cstr(name, true);

  if (!hp) {
    return PH_ERR;
  }

  return do_register(hp, func, closure, pri, unreg);
}

ph_result_t ph_hook_unregister(ph_string_t *name, ph_hook_func func,
    void *closure)
{
  ph_hook_point_t *hp = ph_hook_point_get(name, true);

  if (!hp) {
    return PH_ERR;
  }

  return do_unregister(hp, func, closure);
}

ph_result_t ph_hook_unregister_cstr(const char *name, ph_hook_func func,
    void *closure)
{
  ph_hook_point_t *hp = ph_hook_point_get_cstr(name, true);

  if (!hp) {
    return PH_ERR;
  }
  return do_unregister(hp, func, closure);
}

void ph_hook_invoke_vargs(ph_hook_point_t *hook, uint8_t nargs, ...)
{
  va_list ap;

  va_start(ap, nargs);
  ph_hook_invokev(hook, nargs, ap);
  va_end(ap);
}

/* vim:ts=2:sw=2:et:
 */


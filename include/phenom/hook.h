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

#ifndef PHENOM_HOOK_H
#define PHENOM_HOOK_H

#include "phenom/string.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_hook_invocation;
typedef struct ph_hook_invocation ph_hook_invocation_t;

typedef void (*ph_hook_func)(ph_hook_invocation_t *inv,
    void *closure, uint8_t nargs, void **args);

typedef void (*ph_hook_unreg_func)(void *closure, ph_hook_func func);

struct ph_hook_item {
  void *closure;
  ph_hook_func func;
  int8_t pri;
  ph_hook_unreg_func unreg;
};
typedef struct ph_hook_item ph_hook_item_t;

struct ph_hook_point_head {
  ck_epoch_entry_t entry;
  uint16_t nitems;
  ph_hook_item_t items[1];
};
typedef struct ph_hook_point_head ph_hook_point_head_t;

struct ph_hook_point {
  ph_hook_point_head_t *head;
};
typedef struct ph_hook_point ph_hook_point_t;

struct ph_hook_invocation {
  bool stopped;
};

/** Resolve a hook point by name
 *
 * If the hook point does not exist and `create` is true, creates
 * the hook point and returns it.
 *
 * Once created, a hook point is never deleted.
 */
ph_hook_point_t *ph_hook_point_get(ph_string_t *name, bool create);

/** Resolve a hook point by c-string name
 *
 * Delegates to ph_hook_point_get().
 */
ph_hook_point_t *ph_hook_point_get_cstr(const char *name, bool create);

/** Invoke a hook
 */
static inline void ph_hook_invoke_inner(ph_hook_point_t *hook,
    uint8_t nargs, void **args)
{
  ph_hook_point_head_t *head = ck_pr_load_ptr(&hook->head);
  uint16_t n;
  ph_hook_invocation_t inv;

  if (!head) {
    return;
  }

  inv.stopped = false;

  for (n = 0; n < head->nitems; n++) {
    head->items[n].func(&inv, head->items[n].closure, nargs, args);
    if (inv.stopped) {
      break;
    }
  }
}

/** Invoke a hook with varargs via va_list
 *
 * Each argument MUST be a pointer to the value in question
 */
#define PH_HOOK_MAX_VARGS 16
static inline void ph_hook_invokev(ph_hook_point_t *hook,
    uint8_t nargs, va_list ap)
{
  uint8_t n;
/* please rethink your API if you need more than this */
  void *args[PH_HOOK_MAX_VARGS];

  ph_debug_assert(nargs < PH_HOOK_MAX_VARGS, "nargs is too big");

  for (n = 0; n < nargs; n++) {
    args[n] = va_arg(ap, void*);
  }

  ph_hook_invoke_inner(hook, nargs, args);
}

/** Invoke a hook with varargs
 *
 * Each argument MUST be a pointer to the value in question
 */
void ph_hook_invoke_vargs(ph_hook_point_t *hook, uint8_t nargs, ...);

#define ph_hook_invoke(hook, nargs, ...) do { \
  ph_debug_assert(nargs < PH_HOOK_MAX_VARGS, "nargs too big"); \
  ph_hook_invoke_vargs(hook, nargs, __VA_ARGS__); \
} while (0)

/** Stop invoking the current hook
 *
 * Prevents any later handlers from being called
 */
static inline void ph_hook_invocation_stop(ph_hook_invocation_t *inv)
{
  inv->stopped = true;
}

#define PH_HOOK_PRI_FIRST (-100)
#define PH_HOOK_PRI_LAST    100

/** Register a hook callback
 *
 * Creates a hook point with the specified name, then registers
 * an entry in the hook item list.
 *
 * Each registration call will add a hook entry, regardless of the currently
 * registered hooks, even if this would cause a duplicate entry to be made.
 *
 * The `unreg` func will be invoked when a hook item is unregistered.
 * This allows for resources to be released at the appropriate time.
 *
 * `pri` specifies the priority of the item; it may be any value in the full
 * range of an `int8_t`.  The convention is to specify a `pri = 0` in the
 * case that you don't care when the item is invoked or `PH_HOOK_PRI_FIRST`
 * if you want to make the item to be the first to be invoked, or
 * `PH_HOOK_PRI_LAST` if you want it to be last.  These constants don't
 * literally map to first or last but put the priority in the right ballpark.
 *
 * The hook items are sorted according to ascending priority value, with the
 * most negative `int8_t` value being the very first to run.  If two items
 * have the same `pri` value then they are run in an unspecified order with
 * respect to each other.  The implementation guarantees that this unspecified
 * order will be consistent for any pair of hook items.  That is, if there
 * are two hook items `tuple(N1, F1, C1)` and `tuple(N1, F2, C2)` that have the
 * same `pri` value, `F1` and `F2` will always be invoked in the same relative
 * order for the lifetime of the process.
 */
ph_result_t ph_hook_register(ph_string_t *name, ph_hook_func func,
    void *closure, int8_t pri, ph_hook_unreg_func unreg);

/** Register a hook callback
 *
 * Delegates to ph_hook_register() but is declared in terms of a C-string
 */
ph_result_t ph_hook_register_cstr(const char *name, ph_hook_func func,
    void *closure, int8_t pri, ph_hook_unreg_func unreg);

/** Unregister a previously registered hook item
 *
 * Returns `PH_OK` if the item was present and has been removed, `PH_ERR`
 * otherwise.
 *
 * If duplicate entries that match `func` and `closure` are present, at
 * most 1 of them will be removed.  It is undefined which will be removed.
 * This preserves the property that for each register call that was made,
 * there should be one call to unregister it.  You should avoid registering
 * duplicates.
 *
 * The `unreg` function associated with the item will be scheduled via
 * ph_thread_epoch_defer(), which typically means that it will be invoked
 * at some pointer after the current stack frame unwinds.
 */
ph_result_t ph_hook_unregister(ph_string_t *name, ph_hook_func func,
    void *closure);

/** Unregister a previously registered hook item
 *
 * Delegates to ph_hook_unreg_func() but is declared in terms of a C-string
 */
ph_result_t ph_hook_unregister_cstr(const char *name, ph_hook_func func,
    void *closure);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


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

#include "phenom/timerwheel.h"

static inline void tval_add_res(ph_timerwheel_t *wheel,
    struct timeval *op1, struct timeval *res)
{
  struct timeval diff;

  diff.tv_sec = wheel->tick_resolution / 1000;
  diff.tv_usec = (wheel->tick_resolution - (diff.tv_sec * 1000)) * 1000;

  timeradd(&diff, op1, res);
}

ph_result_t ph_timerwheel_init(
    ph_timerwheel_t *wheel,
    struct timeval now,
    uint32_t tick_resolution)
{
  int i;

  ck_rwlock_init(&wheel->lock);
  wheel->tick_resolution = tick_resolution;

  tval_add_res(wheel, &now, &wheel->next_run);

  for (i = 0; i < PHENOM_WHEEL_SIZE; i++) {
    PH_LIST_INIT(&wheel->buckets[0].lists[i]);
    PH_LIST_INIT(&wheel->buckets[1].lists[i]);
    PH_LIST_INIT(&wheel->buckets[2].lists[i]);
    PH_LIST_INIT(&wheel->buckets[3].lists[i]);
  }

  return PH_OK;
}

static inline uint64_t tval_to_tick(ph_timerwheel_t *wheel, struct timeval tv)
{
  return (tv.tv_sec * 1000 / wheel->tick_resolution) +
         (tv.tv_usec / (wheel->tick_resolution * 1000));
}

static inline struct ph_timerwheel_list *compute_list(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  struct ph_timerwheel_list *list;
  uint64_t due, now, diff;

  // Ensure that we never schedule in the past
  if (timercmp(&timer->due, &wheel->next_run, <)) {
    memcpy(&timer->due, &wheel->next_run, sizeof(timer->due));
  }

  now = tval_to_tick(wheel, wheel->next_run);
  due = tval_to_tick(wheel, timer->due);
  diff = due - now;

  if (diff < PHENOM_WHEEL_SIZE) {
    list = wheel->buckets[0].lists + (due & PHENOM_WHEEL_MASK);
  } else if (diff < 1 << (2 * PHENOM_WHEEL_BITS)) {
    list = wheel->buckets[1].lists +
            ((due >> PHENOM_WHEEL_BITS) & PHENOM_WHEEL_MASK);
  } else if (diff < 1 << (3 * PHENOM_WHEEL_BITS)) {
    list = wheel->buckets[2].lists +
            ((due >> (2*PHENOM_WHEEL_BITS)) & PHENOM_WHEEL_MASK);
  } else {
    /* in largest slot */
    if (diff > 0xffffffffLL) {
      diff = 0xffffffffLL;
      due = diff + now;
    }
    list = wheel->buckets[3].lists +
            ((due >> (3*PHENOM_WHEEL_BITS)) & PHENOM_WHEEL_MASK);
  }

  return list;
}

ph_result_t ph_timerwheel_enable(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  struct ph_timerwheel_list *list;

  if (!ck_pr_cas_int(&timer->enable, PH_TIMER_DISABLED, PH_TIMER_LOCKED)) {
    return PH_BUSY;
  }

  if (ck_pr_load_ptr(&timer->list)) {
    ck_rwlock_read_lock(&wheel->lock);
    {
      list = compute_list(wheel, timer);
      if (list == timer->list) {
        ck_pr_store_int(&timer->enable, PH_TIMER_ENABLED);
        ck_rwlock_read_unlock(&wheel->lock);
        return PH_OK;
      }
    }
    ck_rwlock_read_unlock(&wheel->lock);
  }

  // Need to recompute as the time may have changed since
  // we released the reader and obtained the writer lock

  ck_rwlock_write_lock(&wheel->lock);
  {
    if (timer->list) {
      PH_LIST_REMOVE(timer, t);
    }
    list = compute_list(wheel, timer);

    timer->list = list;
    ck_pr_store_int(&timer->enable, PH_TIMER_ENABLED);
    PH_LIST_INSERT_HEAD(list, timer, t);
  }
  ck_rwlock_write_unlock(&wheel->lock);

  return PH_OK;
}

ph_result_t ph_timerwheel_remove(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  ph_result_t res;

  if (!ck_pr_load_ptr(&timer->list)) {
    return PH_NOENT;
  }

  res = ph_timerwheel_disable(wheel, timer);
  if (res != PH_OK) {
    return res;
  }

  ck_rwlock_write_lock(&wheel->lock);
  {
    PH_LIST_REMOVE(timer, t);
    ck_pr_store_ptr(&timer->list, 0);
  }
  ck_rwlock_write_unlock(&wheel->lock);

  return PH_OK;
}

/* returns true if we should cascade to the next level,
 * which is in the case where our slot is 0 */
static bool cascade_timer(ph_timerwheel_t *wheel,
    struct ph_timerwheel_list *from, int slot)
{
  struct ph_timerwheel_list list, *target;
  struct ph_timerwheel_timer *timer, *tmp;

  /* steal all items from the the origin list */
  PH_LIST_INIT(&list);
  PH_LIST_SWAP(&list, from + slot, ph_timerwheel_timer, t);

  /* "re"-schedule the timers, putting them into the correct
   * slots */
  PH_LIST_FOREACH_SAFE(timer, &list, t, tmp) {
    /* we're called under the wheel.lock */
    PH_LIST_REMOVE(timer, t);

    if (ck_pr_load_int(&timer->enable) == PH_TIMER_DISABLED) {
      // They'll need to recompute when they are enabled anyway,
      // so don't both re-inserting now
      ck_pr_store_ptr(&timer->list, 0);
      continue;
    }

    target = compute_list(wheel, timer);
    timer->list = target;
    PH_LIST_INSERT_HEAD(target, timer, t);
  }

  return slot == 0;
}

uint32_t ph_timerwheel_tick(
    ph_timerwheel_t *wheel,
    struct timeval now,
    ph_timerwheel_should_dispatch_func_t should_dispatch,
    ph_timerwheel_dispatch_func_t dispatch,
    void *arg)
{
  struct ph_timerwheel_list list;
  struct ph_timerwheel_timer *timer, *tmptimer;
  int idx;
  uint64_t tick, nowtick;
  uint32_t ticked = 0;

  tick = tval_to_tick(wheel, now);
  nowtick = tval_to_tick(wheel, wheel->next_run);

  PH_LIST_INIT(&list);

  ck_rwlock_write_lock(&wheel->lock);
  {
    if (nowtick <= tick) {
      idx = nowtick & PHENOM_WHEEL_MASK;

      if (idx == 0) {
        /* it's time to cascade timers */
        if (cascade_timer(wheel, wheel->buckets[1].lists,
              (nowtick >> PHENOM_WHEEL_BITS)
              & PHENOM_WHEEL_MASK) &&

            cascade_timer(wheel, wheel->buckets[2].lists,
              (nowtick >> (2*PHENOM_WHEEL_BITS))
              & PHENOM_WHEEL_MASK)) {
          cascade_timer(wheel, wheel->buckets[3].lists,
            (nowtick >> (3*PHENOM_WHEEL_BITS))
            & PHENOM_WHEEL_MASK);
        }
      }

      tval_add_res(wheel, &wheel->next_run, &wheel->next_run);

      /* claim the timers */
      PH_LIST_SWAP(&list, &wheel->buckets[0].lists[idx],
          ph_timerwheel_timer, t);

      PH_LIST_FOREACH_SAFE(timer, &list, t, tmptimer) {
        bool disp = true;

        if (!ck_pr_cas_int(&timer->enable, PH_TIMER_ENABLED,
              PH_TIMER_LOCKED)) {
          // Wasn't enabled, so we remove it and skip it
          disp = false;
        } else if (should_dispatch && !should_dispatch(wheel,
              timer, now, arg)) {
          disp = false;
        }

        if (!disp) {
          PH_LIST_REMOVE(timer, t);
          ck_pr_store_ptr(&timer->list, 0);
        }
      }
    }
  }
  ck_rwlock_write_unlock(&wheel->lock);

  // Everything in list is in PH_TIMER_LOCKED state
  for (;;) {
    timer = PH_LIST_FIRST(&list);
    if (!timer) break;

    ticked++;

    PH_LIST_REMOVE(timer, t);
    timer->list = NULL;
    timer->enable = PH_TIMER_DISABLED;

    dispatch(wheel, timer, now, arg);
  }

  return ticked;
}

/* vim:ts=2:sw=2:et:
 */


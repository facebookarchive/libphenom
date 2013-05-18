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

ph_result_t ph_timerwheel_init(
    ph_timerwheel_t *wheel,
    ph_time_t now,
    uint32_t tick_resolution)
{
  int i;

  ck_spinlock_init(&wheel->lock);
  wheel->tick_resolution = tick_resolution;
  wheel->next_run = now / wheel->tick_resolution;

  for (i = 0; i < PHENOM_WHEEL_SIZE; i++) {
    PH_LIST_INIT(&wheel->buckets[0].lists[i]);
    PH_LIST_INIT(&wheel->buckets[1].lists[i]);
    PH_LIST_INIT(&wheel->buckets[2].lists[i]);
    PH_LIST_INIT(&wheel->buckets[3].lists[i]);
  }

  return PH_OK;
}

ph_result_t ph_timerwheel_insert(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  ph_result_t res;

  ck_spinlock_lock(&wheel->lock);
  ck_pr_faa_32(&timer->generation, 1);
  res = ph_timerwheel_insert_unlocked(wheel, timer);
  ck_spinlock_unlock(&wheel->lock);

  return res;
}

ph_result_t ph_timerwheel_insert_unlocked(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  ph_time_t due = timer->due / wheel->tick_resolution;
  struct ph_timerwheel_list *list;
  int64_t diff = due - wheel->next_run;

  if (diff < 0) {
    // Ensure that we never schedule in the past
    due = wheel->next_run;
    diff = 0;
  }
  if (diff < PHENOM_WHEEL_SIZE) {
    list = wheel->buckets[0].lists + (due & PHENOM_WHEEL_MASK);
  } else if (diff < 1 << (2 * PHENOM_WHEEL_BITS)) {
    list = wheel->buckets[1].lists +
            ((due >> PHENOM_WHEEL_BITS) & PHENOM_WHEEL_MASK);
  } else if (diff < 1 << (3 * PHENOM_WHEEL_BITS)) {
    list = wheel->buckets[2].lists +
            ((due >> (2*PHENOM_WHEEL_BITS)) & PHENOM_WHEEL_MASK);
  } else if (diff < 0) {
    /* overdue */
    list = wheel->buckets[0].lists + (wheel->next_run & PHENOM_WHEEL_MASK);
  } else {
    /* in largest slot */
    if (diff > 0xffffffffLL) {
      diff = 0xffffffffLL;
      due = diff + wheel->next_run;
    }
    list = wheel->buckets[3].lists +
            ((due >> (3*PHENOM_WHEEL_BITS)) & PHENOM_WHEEL_MASK);
  }

  PH_LIST_INSERT_HEAD(list, timer, t);

  return PH_OK;
}

ph_result_t ph_timerwheel_remove(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  ph_result_t res;

  ck_spinlock_lock(&wheel->lock);
  ck_pr_faa_32(&timer->generation, 1);
  res = ph_timerwheel_remove_unlocked(wheel, timer);
  ck_spinlock_unlock(&wheel->lock);

  return res;
}

ph_result_t ph_timerwheel_remove_unlocked(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  unused_parameter(wheel);

  PH_LIST_REMOVE(timer, t);

  return PH_OK;
}

/* returns true if we should cascade to the next level,
 * which is in the case where our slot is 0 */
static bool cascade_timer(ph_timerwheel_t *wheel,
    struct ph_timerwheel_list *from, int slot)
{
  struct ph_timerwheel_list list;
  struct ph_timerwheel_timer *timer, *tmp;

  /* steal all items from the the origin list */
  PH_LIST_INIT(&list);
  PH_LIST_SWAP(&list, from + slot, ph_timerwheel_timer, t);

  /* "re"-schedule the timers, putting them into the correct
   * slots */
  PH_LIST_FOREACH_SAFE(timer, &list, t, tmp) {
    /* we're called under the wheel.lock */
    PH_LIST_REMOVE(timer, t);
    ph_timerwheel_insert_unlocked(wheel, timer);
  }

  return slot == 0;
}

bool ph_timerwheel_timer_was_modified(
    struct ph_timerwheel_timer *timer)
{
  return ck_pr_load_32(&timer->generation) !=
    ck_pr_load_32(&timer->wheel_gen);
}

uint32_t ph_timerwheel_tick(
    ph_timerwheel_t *wheel,
    ph_time_t now,
    ph_timerwheel_dispatch_func_t dispatch,
    void *arg)
{
  struct ph_timerwheel_list list;
  struct ph_timerwheel_timer *timer;
  int idx;
  int64_t tick;
  uint32_t ticked = 0;

  tick = now / wheel->tick_resolution;

  PH_LIST_INIT(&list);

  ck_spinlock_lock(&wheel->lock);
  {
    if (wheel->next_run <= tick) {
      idx = wheel->next_run & PHENOM_WHEEL_MASK;

      if (idx == 0) {
        /* it's time to cascade timers */
        if (cascade_timer(wheel, wheel->buckets[1].lists,
              (wheel->next_run >> PHENOM_WHEEL_BITS)
              & PHENOM_WHEEL_MASK) &&

            cascade_timer(wheel, wheel->buckets[2].lists,
              (wheel->next_run >> (2*PHENOM_WHEEL_BITS))
              & PHENOM_WHEEL_MASK)) {

          cascade_timer(wheel, wheel->buckets[3].lists,
            (wheel->next_run >> (3*PHENOM_WHEEL_BITS))
            & PHENOM_WHEEL_MASK);
        }
      }

      wheel->next_run++;

      PH_LIST_FOREACH(timer, &wheel->buckets[0].lists[idx], t) {
        /* observe the generation number so we can
         * detect a change in the timer state */
        ck_pr_store_32(&timer->wheel_gen,
            ck_pr_load_32(&timer->generation));
      }
      /* claim the timers */
      PH_LIST_SWAP(&list, &wheel->buckets[0].lists[idx],
          ph_timerwheel_timer, t);
    }
  }
  ck_spinlock_unlock(&wheel->lock);

  for (;;) {
    timer = PH_LIST_FIRST(&list);
    if (!timer) break;

    ticked++;
    PH_LIST_REMOVE(timer, t);

    if (!ph_timerwheel_timer_was_modified(timer)) {
      dispatch(wheel, timer, now, arg);
    }
  }

  return ticked;
}

/* vim:ts=2:sw=2:et:
 */


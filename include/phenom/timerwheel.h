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
 * # Timer Wheel
 *
 * This borrows from the concepts explored in the paper:
 * "Hashed and Hierarchical Timing Wheels: Effcient
 * Data Structures for Implementing a Timer Facility by
 * George Varghese and Anthony Lauck"
 *
 * We model timers as the number of ticks until the next
 * due event.  We allow 32-bits of space to track this
 * due interval, and break that into 4 regions of 8 bits.
 * Each region indexes into a bucket of 256 lists.
 *
 * Bucket 0 represents those events that are due the soonest.
 * Each tick causes us to look at the next list in a bucket.
 * The 0th list in a bucket is special; it means that it is time to
 * flush the timers from the next higher bucket and schedule them
 * into a different bucket.
 *
 * This technique results in a very cheap mechanism for
 * maintaining time and timers, provided that we can maintain
 * a consistent rate of ticks.
 */

#ifndef PHENOM_TIMERWHEEL_H
#define PHENOM_TIMERWHEEL_H

#include <phenom/defs.h>
#include <phenom/thread.h>
#include <phenom/queue.h>
#include <ck_rwlock.h>

#ifdef __cplusplus
extern "C" {
#endif

PH_LIST_HEAD(
    ph_timerwheel_list,
    ph_timerwheel_timer);

struct ph_timerwheel_timer {
  PH_LIST_ENTRY(ph_timerwheel_timer) t;
  struct ph_timerwheel_list *list;
  struct timeval due;
  int enable;
#define PH_TIMER_DISABLED    0
#define PH_TIMER_ENABLED     1
#define PH_TIMER_LOCKED      2
};

#define PHENOM_WHEEL_BITS 8
#define PHENOM_WHEEL_SIZE (1 << PHENOM_WHEEL_BITS)
#define PHENOM_WHEEL_MASK (PHENOM_WHEEL_SIZE - 1)

struct ph_timerwheel {
  struct timeval next_run;
  uint32_t tick_resolution;
  ck_rwlock_t lock;
  struct {
    struct ph_timerwheel_list lists[PHENOM_WHEEL_SIZE];
  } buckets[4];
};

typedef struct ph_timerwheel ph_timerwheel_t;

/** Initialize a timerwheel
 * tick_resolution specifies how many milliseconds comprise a tick.
 */
ph_result_t ph_timerwheel_init(
    ph_timerwheel_t *wheel,
    struct timeval now,
    uint32_t tick_resolution);

/** Disable a timer that is already in the timerwheel.
 * It remains in the timerwheel until removed.
 * You may re-enable it using ph_timerwheel_enable().
 * If the wheel ticks and dispatches a disabled timer,
 * it simply skips over it.
 */
static inline ph_result_t ph_timerwheel_disable(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer)
{
  ph_unused_parameter(wheel);

  if (ck_pr_load_int(&timer->enable) == PH_TIMER_DISABLED) {
    return PH_OK;
  }

  if (ck_pr_cas_int(&timer->enable, PH_TIMER_ENABLED, PH_TIMER_DISABLED)) {
    return PH_OK;
  }

  return PH_BUSY;
}


/** Enable a timer.
 * Can be used to insert a new timer or re-enable a disabled timer.
 * */
ph_result_t ph_timerwheel_enable(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer);

/** Remove an element from the timerwheel */
ph_result_t ph_timerwheel_remove(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer);

/* Called by the wheel to advise that a timer will be dispatched
 * imminently.  This gives you the opportunity to take steps to make
 * this safe.  Return true to allow the timer to be dispatched or
 * false to ignore it.  Returning false deactivates the timer.
 * This is called under the wheel lock */
typedef bool (*ph_timerwheel_should_dispatch_func_t)(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg);

/* Called by the wheel to actually dispatch a timer */
typedef void (*ph_timerwheel_dispatch_func_t)(
    ph_timerwheel_t *wheel,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg);

/** Tick and dispatch any due timer(s).
 *
 * You must arrange for the wheel to tick over at least
 * once every tick_resolution milliseconds to avoid
 * falling behind.
 *
 * You supply the current time when you call this function.
 * The wheel will tick through and dispatch any due (or overdue!)
 * timers by invoking your dispatch function.
 *
 * Returns the number of timers that were dispatched.
 */
uint32_t ph_timerwheel_tick(
    ph_timerwheel_t *wheel,
    struct timeval now,
    ph_timerwheel_should_dispatch_func_t should_dispatch,
    ph_timerwheel_dispatch_func_t dispatch,
    void *arg);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


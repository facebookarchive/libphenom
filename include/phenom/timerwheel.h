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

/* This borrows from the concepts explored in the paper:
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
#include <ck_spinlock.h>

#ifdef __cplusplus
extern "C" {
#endif

PH_LIST_HEAD(
    phenom_timerwheel_list,
    phenom_timerwheel_timer);

struct phenom_timerwheel_timer {
  PH_LIST_ENTRY(phenom_timerwheel_timer) t;
  phenom_time_t due;
  uint32_t generation, wheel_gen;
};

#define PHENOM_WHEEL_BITS 8
#define PHENOM_WHEEL_SIZE (1 << PHENOM_WHEEL_BITS)
#define PHENOM_WHEEL_MASK (PHENOM_WHEEL_SIZE - 1)

struct phenom_timerwheel {
  int64_t next_run;
  uint32_t tick_resolution;
  ck_spinlock_t lock;
  struct {
    struct phenom_timerwheel_list lists[PHENOM_WHEEL_SIZE];
  } buckets[4];
};

typedef struct phenom_timerwheel phenom_timerwheel_t;

/** Initialize a timerwheel
 * tick_resolution specifies how many milliseconds comprise a tick.
 */
phenom_result_t phenom_timerwheel_init(
    phenom_timerwheel_t *wheel,
    phenom_time_t now,
    uint32_t tick_resolution);

/** Insert an element into the timerweel */
phenom_result_t phenom_timerwheel_insert(
    phenom_timerwheel_t *wheel,
    struct phenom_timerwheel_timer *timer);

phenom_result_t phenom_timerwheel_insert_unlocked(
    phenom_timerwheel_t *wheel,
    struct phenom_timerwheel_timer *timer);

/** Remove an element from the timerwheel */
phenom_result_t phenom_timerwheel_remove(
    phenom_timerwheel_t *wheel,
    struct phenom_timerwheel_timer *timer);

phenom_result_t phenom_timerwheel_remove_unlocked(
    phenom_timerwheel_t *wheel,
    struct phenom_timerwheel_timer *timer);

typedef void (*phenom_timerwheel_dispatch_func_t)(
    phenom_timerwheel_t *wheel,
    struct phenom_timerwheel_timer *timer,
    phenom_time_t now,
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
uint32_t phenom_timerwheel_tick(
    phenom_timerwheel_t *wheel,
    phenom_time_t now,
    phenom_timerwheel_dispatch_func_t dispatch,
    void *arg);

bool phenom_timerwheel_timer_was_modified(
    struct phenom_timerwheel_timer *timer);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


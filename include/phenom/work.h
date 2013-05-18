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

#ifndef PHENOM_WORK_H
#define PHENOM_WORK_H

#include "phenom/defs.h"
#include "phenom/thread.h"
#include "phenom/timerwheel.h"
#include "phenom/sysutil.h"

#ifdef __cplusplus
extern "C" {
#endif

// Triggered by being IO-ready.
// triggerdata is the event mask
#define PH_TRIGGER_IO       1

// Triggered by timeout
#define PH_TRIGGER_TIMEOUT  2

// Triggered by something generic
#define PH_TRIGGER_GENERIC  3

// IO event mask
typedef uint32_t ph_io_mask_t;
#define PH_IO_MASK_NONE    0
#define PH_IO_MASK_READ    1
#define PH_IO_MASK_WRITE   2
#define PH_IO_MASK_ERR     4

typedef void (*ph_work_func_t)(
    ph_work_item_t *work,
    // how we were triggered
    uint32_t trigger,
    // the current time
    ph_time_t now,
    // work->data
    void *workdata,
    // interpret depending on trigger source
    intptr_t triggerdata
);

struct ph_work_item {
  // indicates how we get dispatched
  int runclass;

  // what to do when we get dispatched
  ph_work_func_t callback;

  // data associated with the item
  void *data;

  // associated file descriptor
  ph_socket_t fd;

// accumulate triggers but don't notify or dispatch
// until the item is enabled again
#define PH_TRIGGER_STATE_PAUSED  0
// accumulate triggers, notify and dispatch as they are
// received
#define PH_TRIGGER_STATE_ENABLED 1
// don't accumulate triggers; discard any that arrive
// while in this state
#define PH_TRIGGER_STATE_DISCARD 2
  uint32_t trigger_state;
  ph_thread_t *owner;

  // affinity
  ph_thread_t *affinity;

  // If we have a timer...
  struct ph_timerwheel_timer timer;

  // queue of ph_work_trigger
  ck_fifo_mpmc_t triggers;
};

/** Disables triggers for the specified work item.
 *
 * Pauses the delivery of triggers for the work item.
 * If the item is currently executing in another thread then
 * PHENOM_BUSY will be returned and no changes will have
 * been made to the item.
 *
 * If DISCARD is true, then any triggers that target the
 * item while it is disabled will be discarded.  Otherwise
 * the triggers will be queued up until the triggers are
 * re-enabled.
 */
ph_result_t ph_work_trigger_disable(
    ph_work_item_t *item,
    bool discard);

/** Enables triggers for the specified work item.
 *
 * Changes will take effect once the currently dispatching
 * work item returns to the scheduler; this makes it simpler
 * to avoid issues where the target triggers while current
 * item is in the middle of setting up multiple events
 */
ph_result_t ph_work_trigger_enable(
    ph_work_item_t *item);

/** Arranges for a work item to be dispatched when IO is ready
 *
 * Attempts to immediately disable all triggers for the specified
 * work item, such that any that arrive will be deferred.
 *
 * If we cannot disable triggers (perhaps the item is executing
 * right now in another thread), then we'll return PHENOM_BUSY.
 *
 * Otherwise, we'll arrange for the work item to be rescheduled
 * with the modified IO event mask.  Any other non-IO triggers
 * that we disabled when we obtained the item will be re-enabled
 * at that time.
 */
ph_result_t ph_work_io_event_mask_set(
    ph_work_item_t *item,
    ph_socket_t fd,
    ph_io_mask_t mask);

/** Arranges for a work item to be dispatched at a certain time.
 *
 * If time is 0, disables any previously arranged timeout trigger.
 *
 * Similar to ph_work_io_event_mask_set(), this function
 * may fail if we are unable to immediately operate on the item.
 */
ph_result_t ph_work_timeout_at(
    ph_work_item_t *item,
    ph_time_t at);

ph_time_t ph_time_now(void);

ph_result_t ph_work_init(
    ph_work_item_t *item);

ph_result_t ph_work_destroy(
    ph_work_item_t *item);


/** arranges for a work item to be dispatched.
 *
 * The dispatch will be propagated to the appropriate
 * dispatch queue and trigger asynchronously from
 * the context of the calling thread.
 */
ph_result_t ph_work_trigger(
    ph_work_item_t *work,
    uint32_t trigger,
    intptr_t triggerdata);

/** Set the dispatch affinity for a work item.
 *
 * Sets the item so that its callback function will
 * only ever be called from the currently executing
 * thread.
 *
 * This is useful in cases where your callback uses
 * libraries that have strong thread affinity requirements.
 *
 * It is better to avoid calling this function if you can!
 */
ph_result_t ph_work_dispatch_affinity_set_current(
    ph_work_item_t *item);


ph_result_t ph_sched_init(uint32_t sched_cores, uint32_t fd_hint);
ph_result_t ph_sched_run(void);
void ph_sched_stop(void);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


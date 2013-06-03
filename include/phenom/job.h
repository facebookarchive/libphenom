/*
 * Copyright 2013 Facebook, Inc.
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

#ifndef PHENOM_JOB_H
#define PHENOM_JOB_H

#include "phenom/defs.h"
#include "phenom/thread.h"
#include "phenom/timerwheel.h"
#include "phenom/sysutil.h"
#include "phenom/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phenom Jobs
 * Jobs describe a parcel of work.  Jobs may be triggered or dispatched
 * in one of a number of "run classes".  There are three run classes:
 *
 * - Immediate.  The work is dispatched immediately on the calling thread.
 * - NBIO.       The work is dispatched when a descriptor is signalled
 *               for I/O.
 * - Pool.       The work is queued to a thread pool and is dispatched
 *               as soon as a worker becomes available.
 *               Phenom allows multiple pools to be defined to better
 *               partition and prioritize your workload.
 */

/** NBIO trigger mask */
typedef uint8_t ph_iomask_t;
/** NBIO is disabled or not applicable */
#define PH_IOMASK_NONE  0
/** NBIO will/did dispatch for readable events */
#define PH_IOMASK_READ  1
/** NBIO will/did dispatch for writable events */
#define PH_IOMASK_WRITE 2
/** NBIO dispatched due to IO error */
#define PH_IOMASK_ERR   4
/** NBIO did not dispatch before timeout was met */
#define PH_IOMASK_TIME  8

#define PH_RUNCLASS_NONE  0
#define PH_RUNCLASS_NBIO  1

struct ph_job;
typedef struct ph_job ph_job_t;

typedef void (*ph_job_func_t)(
    ph_job_t *job,
    ph_iomask_t why,
    void *data
);

struct ph_job {
  /** data associated with job */
  void *data;
  ph_job_func_t callback;
  /** deferred apply list */
  PH_STAILQ_ENTRY(ph_job) q_ent;

  /** for PH_RUNCLASS_NBIO, trigger mask */
  ph_iomask_t mask;
  ph_socket_t fd;
  struct timeval   timeout;
  struct ph_timerwheel_timer timer;
  ph_thread_t *owner;
  uint32_t vers;
  uint32_t tvers;

  ph_thread_pool_t *pool;
};

/** Initializes a job structure.
 *
 * We allow for jobs to be embedded in a container structure.
 * This function correctly initializes the job.
 */
ph_result_t ph_job_init(ph_job_t *job);

/** Destroys a job structure.
 *
 * Correctly releases resources associated with a job.
 */
ph_result_t ph_job_destroy(ph_job_t *job);

/** Configure a job for NBIO.
 */
ph_result_t ph_job_set_nbio(
    ph_job_t *job,
    ph_iomask_t mask,
    struct timeval *timeout);

/** Configure a job to run at a specific time */
ph_result_t ph_job_set_timer_at(
    ph_job_t *job,
    struct timeval abstime);

/** Configure a job to run after an interval */
ph_result_t ph_job_set_timer_in(
    ph_job_t *job,
    struct timeval interval);

/** Configure a job to run after an interval expressed in milliseconds */
ph_result_t ph_job_set_timer_in_ms(
    ph_job_t *job,
    uint32_t interval);

/** Configure a job for pooled use and queue it to the
 * pool.  It will be dispatched when the current dispatch
 * frame is unwound.
 */
ph_result_t ph_job_set_pool(
    ph_job_t *job,
    ph_thread_pool_t *pool);

/** Configure a job for pooled use and queue it to the
 * pool.  It will be dispatched during or after the
 * the call to ph_job_set_pool_immediate returns.  Use
 * with caution as it is easy to experience race conditions
 * with the job finishing before you're done preparing for
 * it to finish.
 */
ph_result_t ph_job_set_pool_immediate(ph_job_t *job,
    ph_thread_pool_t *pool);

/** Define a new job pool
 *
 * The pool is created in an offline state and will be brought
 * online when it is first assigned a job via ph_job_set_pool().
 * max_queue_len is used to size the producer ring buffers.  If
 * a ring buffer is full, this function will block until room
 * becomes available.
 *
 * max_queue_len defines the upper bound on the number of items that can
 * be queued to the producer queue associated with the current
 * thread.  There is no pool-wide maximum limit (it is too expensive
 * to maintain and enforce), but there is a theoretical upper bound
 * of MAX(4, ph_power_2(max_queue_len)) * 64 jobs that can be "queued",
 * assuming that all 63 preferred threads and all the non-preferred
 * threads are busy saturating the pool.
 */
ph_thread_pool_t *ph_thread_pool_define(
    const char *name,
    uint32_t max_queue_len,
    uint32_t num_threads
);

/** Resolve a thread pool by name.
 * This is O(number-of-pools); you should cache the result.
 */
ph_thread_pool_t *ph_thread_pool_by_name(const char *name);

/** thread pool stats.
 * These are accumulated using ph_counter under the covers.
 * This means that the numbers are a snapshot across a number
 * of per-thread views.
 */
struct ph_thread_pool_stats {
  // Number of jobs that have been dispatched
  int64_t num_dispatched;
  // How many times a worker thread has gone to sleep
  int64_t consumer_sleeps;
  // How many times a producer has been blocked by a full
  // local ring buffer and gone to sleep
  int64_t producer_sleeps;
};

/** Return thread pool counters for a given pool */
void ph_thread_pool_stat(ph_thread_pool_t *pool,
    struct ph_thread_pool_stats *stats);

ph_result_t ph_nbio_init(uint32_t sched_cores);
ph_result_t ph_job_pool_init(void);

/* ----
 * the following are implementation specific and shouldn't
 * be called except by wizards
 */
void ph_job_pool_shutdown(void);
void ph_job_pool_apply_deferred_items(ph_thread_t *me);
ph_result_t ph_sched_run(void);
void ph_sched_stop(void);

void _ph_job_set_pool_immediate(ph_job_t *job, ph_thread_t *me);
void _ph_job_pool_start_threads(void);

static inline bool ph_job_have_deferred_items(ph_thread_t *me)
{
  return PH_STAILQ_FIRST(&me->pending_dispatch);
}

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


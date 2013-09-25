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

#ifndef PHENOM_JOB_H
#define PHENOM_JOB_H

#include "phenom/defs.h"
#include "phenom/thread.h"
#include "phenom/timerwheel.h"
#include "phenom/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * # Jobs
 * Jobs describe a parcel of work.  Jobs may be triggered or dispatched
 * in one of a number of "run classes".  There are three run classes:
 *
 * - Immediate. The work is dispatched immediately on the calling thread.
 * - NBIO. The work is dispatched when a descriptor is signalled for I/O.
 * - Pool. The work is queued to a thread pool and is dispatched as soon as a
 *   worker becomes available.  libPhenom allows multiple pools to be defined to
 *   better partition and prioritize your workload.
 */

/* NBIO trigger mask */
typedef uint8_t ph_iomask_t;
/* NBIO is disabled or not applicable */
#define PH_IOMASK_NONE  0
/* NBIO will/did dispatch for readable events */
#define PH_IOMASK_READ  1
/* NBIO will/did dispatch for writable events */
#define PH_IOMASK_WRITE 2
/* NBIO dispatched due to IO error */
#define PH_IOMASK_ERR   4
/* NBIO did not dispatch before timeout was met */
#define PH_IOMASK_TIME  8

struct ph_job;
typedef struct ph_job ph_job_t;

typedef void (*ph_job_func_t)(
    ph_job_t *job,
    ph_iomask_t why,
    void *data
);

struct ph_job_def {
  ph_job_func_t callback;
  ph_memtype_t memtype;
  void (*dtor)(ph_job_t *job);
};

/** Job
 * Use either ph_job_alloc() to allocate and initialize, or allocate it yourself
 * and use ph_job_init() to initialize the fields.
 */
struct ph_job {
  // data associated with job
  void *data;
  // the callback to run when the job is dispatched
  ph_job_func_t callback;
  // deferred apply list
  PH_STAILQ_ENTRY(ph_job) q_ent;
  bool in_apply;
  // for PH_RUNCLASS_NBIO, trigger mask */
  ph_iomask_t mask;
  // use ph_job_get_kmask() to interpret
  int kmask;
  // Hashed over the scheduler threads; two jobs with
  // the same emitter hash will run serially wrt. each other
  uint32_t emitter_affinity;
  // For nbio, the socket we're bound to for IO events
  ph_socket_t fd;
  // Holds timeout state
  struct ph_timerwheel_timer timer;
  // When targeting a thread pool, which pool
  ph_thread_pool_t *pool;
  // for SMR
  ck_epoch_entry_t epoch_entry;
  struct ph_job_def *def;
};

/** Initializes a job structure.
 *
 * We allow for jobs to be embedded in a container structure.
 * This function correctly initializes the job.
 */
ph_result_t ph_job_init(ph_job_t *job);

/** Allocates a job structure using a template
 *
 * A common case is to embed the job at the head of a struct and
 * to manage that whole struct in a memtype based allocation.
 *
 * This function will allocate and initialize a job using the
 * provided template; the template specifies a memtype to use for
 * the allocation (it must be a fixed size memtype) and a default value
 * for the callback function.
 *
 * When the job is no longer needed, you should call ph_job_free()
 * to arrange for it to be freed during a grace period.
 */
ph_job_t *ph_job_alloc(struct ph_job_def *def);

/** Arranges to free a templated job structure
 *
 * The dtor from your job template will be invoked at a safe point.
 * You should treat the job as having been freed as soon as this
 * function returns.
 */
void ph_job_free(ph_job_t *job);

/** Configure a job for NBIO.
 */
ph_result_t ph_job_set_nbio(
    ph_job_t *job,
    ph_iomask_t mask,
    struct timeval *abstime);

/** Configure a job for NBIO with a relative timeout */
ph_result_t ph_job_set_nbio_timeout_in(
    ph_job_t *job,
    ph_iomask_t mask,
    struct timeval interval);

/** Returns the currently active iomask
 *
 * This is useful in some situations where you want to know
 * if the job is scheduled in the NBIO scheduler.
 *
 * This API may change as it feels a bit klunky
 */
ph_iomask_t ph_job_get_kmask(ph_job_t *job);

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
 * threads are busy saturating the pool.  On 32-bit systems, the multiplier
 * is 32 instead of 64 and the preferred ring count is 31 instead of 63.
 *
 * Note that the actual values used for `max_queue_len` and `num_threads`
 * will be taken from the configuration values `$.threadpool.NAME.queue_len`
 * and `$.threadpool.NAME.num_threads` respectively, where `NAME` is
 * replaced by the `name` parameter you specify.
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

/**
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

/* io scheduler thread pool stats */
struct ph_nbio_stats {
  /* how many threads are servicing NBIO */
  int num_threads;
  /* how many NBIO dispatches have happened */
  int64_t num_dispatched;
  /* how many timer ticks since process start (~1 every 100ms) */
  int64_t timer_ticks;
  /* how many timer vs. event dispatch conflicts were detected,
   * should be rare */
  int64_t timer_busy;
};

void ph_nbio_stat(struct ph_nbio_stats *stats);

/** Start the run loop.  Must be called from the main thread */
ph_result_t ph_sched_run(void);

/** Requests that the run loop be halted.
 * Can be called from any thread */
void ph_sched_stop(void);

/* ----
 * the following are implementation specific and shouldn't
 * be called except by wizards
 */
void ph_job_pool_shutdown(void);
void ph_job_pool_apply_deferred_items(ph_thread_t *me);

/** Initialize the NBIO pool
 *
 * This MUST be called prior to setting any nbio jobs.
 * `sched_cores` specifies how many threads should be used for
 * NBIO.  Setting it to `0` selects a reasonable default based
 * on some experimentation of the core library.
 *
 * The actual value used for sched_cores will be taken from
 * the configuration for `$.nbio.sched_cores`, if present,
 * otherwise your sched_cores parameter will be used.
 *
 * Other applicable parameters:
 *
 * `$.nbio.epoch_interval` specifies how often we'll schedule a
 * call to ph_thread_epoch_barrier().  The configuration is specified
 * in milliseconds.  If you enabled Gimli support, libphenom will
 * update the heartbeat after performing the barrier.  This ensures
 * that all worker threads are healthy and making progress.
 * The default value for this `5000` milliseconds; it should be
 * more frequent than your Gimli watchdog interval.  You may disable
 * barrier and heartbeat by setting this option to `0`.
 */
ph_result_t ph_nbio_init(uint32_t sched_cores);

ph_result_t ph_job_pool_init(void);

void _ph_job_set_pool_immediate(ph_job_t *job, ph_thread_t *me);
void _ph_job_pool_start_threads(void);

static inline bool ph_job_have_deferred_items(ph_thread_t *me)
{
  return PH_STAILQ_FIRST(&me->pending_nbio) ||
         PH_STAILQ_FIRST(&me->pending_pool);
}

#ifdef PHENOM_IMPL
#include "phenom/timerwheel.h"
#include "phenom/counter.h"
#ifdef HAVE_KQUEUE
struct ph_nbio_kq_set {
  int size;
  int used;
  struct kevent *events;
  struct kevent base[16];
};
#endif
struct ph_nbio_emitter {
  ph_timerwheel_t wheel;
  ph_job_t timer_job;
  uint32_t emitter_id;
  int io_fd, timer_fd;
#ifdef HAVE_PORT_CREATE
  timer_t port_timer;
#endif
  ph_thread_t *thread;
  ph_counter_block_t *cblock;
#ifdef HAVE_KQUEUE
  struct ph_nbio_kq_set kqset;
#endif
};

#define SLOT_DISP 0
#define SLOT_TIMER_TICK 1
#define SLOT_BUSY 2
// We use 100ms resolution
#define WHEEL_INTERVAL_MS 100

void ph_nbio_emitter_init(struct ph_nbio_emitter *emitter);
ph_result_t ph_nbio_emitter_apply_io_mask(struct ph_nbio_emitter *emitter,
    ph_job_t *job, ph_iomask_t mask);
void ph_nbio_emitter_run(struct ph_nbio_emitter *emitter, ph_thread_t *me);

void ph_nbio_emitter_timer_tick(struct ph_nbio_emitter *emitter);
void ph_nbio_emitter_dispatch_immediate(struct ph_nbio_emitter *emitter,
    ph_job_t *job, ph_iomask_t why);

extern int _ph_run_loop;

#endif


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


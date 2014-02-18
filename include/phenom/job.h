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
 *
 * # Affinity
 *
 * By default, threads created by ph_thread_pool_define() and threads in the
 * NBIO pool use an affinity policy that spreads out the threads and binds
 * them to the CPU cores based on their phenom `tid`.
 *
 * Each thread with a phenom TLS segment is assigned a `tid` starting with 0
 * for the main thread of the process, increasing by 1 for each additional
 * thread.
 *
 * Each thread in a thread pool has a `wid` starting at 1 for the "first"
 * thread in that pool, increasing by 1 for each additional thread.
 *
 * The default affinity selector is based on the `tid` such that a process
 * that uses libphenom exclusively for its threads maps each thread across
 * the set of CPU cores.
 *
 * In more complex environments, there may be threads created by other
 * libraries and the overall affinity configuration is made more complex.
 * To cater for this, you may specify the affinity selection in your phenom
 * configuration file.
 *
 * For the NBIO pool:
 *
 * ```
 * {
 *   "nbio": {
 *     "affinity": {
 *       "base": 0,
 *       "selector": "tid"
 *     }
 *   }
 * }
 * ```
 *
 * For any other thread pool, for example, the "MYNAME" pool:
 *
 * ```
 * {
 *   "threadpool": {
 *     "MYNAME": {
 *       "affinity": {
 *         "base": 0,
 *         "selector": "tid"
 *       }
 *     }
 *   }
 * }
 * ```
 *
 * The `base` parameter specifies the offset of the first CPU to bind.
 * The default is CPU `0`.
 *
 * Possible values for `selector` are:
 *
 * * `tid` - binds to `(base + thr->tid) % cores`
 * * `wid` - binds to `(base + thr->wid - 1) % cores`
 * * `none` - does not set CPU affinity
 * * `[1,2,3]` allows the thread to bind to any CPU in the set
 *   `[base + 1, base + 2, base + 3]`.  This specifies an affinity mask, so all
 *   of the threads in this pool will be able to run on any of the CPUs in the
 *   specified set.  libPhenom currently only supports this form on Linux and
 *   BSDish systems; other platforms will bind to the first CPU in the set.
 * * `2` - binds to CPU `base + 2`.  This causes all threads in the pool to
 *   bind to the specified CPU.
 *
 * If you need to bind a `POOL1` to processors 0-3 and `POOL2` to processors
 * 4-8, you'd set your configuration like this:
 *
 * ```
 * {
 *   "threadpool": {
 *     "POOL1": {
 *       "affinity": { "base": 0, "selector": "wid" },
 *       "num_threads": 4
 *     },
 *     "POOL2": {
 *       "affinity": { "base": 4, "selector": "wid" },
 *       "num_threads": 4
 *     },
 *   }
 * }
 * ```
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
/* Dispatch triggered by ph_job_wakeup */
#define PH_IOMASK_WAKEUP 16

struct ph_job;
typedef struct ph_job ph_job_t;

typedef void (*ph_job_func_t)(
    ph_job_t *job,
    ph_iomask_t why,
    void *data
);

/** Job definition
 *
 * Use this to define a template for a job and then use ph_job_alloc() to allocate
 * and safely dispose of the job via ph_job_free().  The template also allows you
 * to pre-initialize the callback for the job.
 */
struct ph_job_def {
  // The callback to run when the job is dispatched.
  // Will be copied to job->callback during ph_job_alloc()
  ph_job_func_t callback;
  // The memtype to use to allocate the job
  ph_memtype_t memtype;
  // Function to be called prior to freeing the job
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
  // whether we're in a deferred apply
  int in_apply;
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
  // Counter of pending wakeups
  uint32_t n_wakeups_pending;
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

/** Clear a previously scheduled timer
 */
ph_result_t ph_job_clear_timer(ph_job_t *job);

/** Dispatch a job immediately in the current context
 */
void ph_job_dispatch_now(ph_job_t *job);

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

/** Signal that a thread pool should stop its workers
 *
 * In some workloads, we want to spawn a pool of threads to process
 * a bunch of jobs.  The number of jobs has a finite upper bound and
 * once they are all complete we no longer need to retain the pool.
 *
 * You may use ph_thread_pool_signal_stop() to instruct the pool
 * to shutdown and stop processing further items.  If you need
 * to synchronize with the shutdown, you may use ph_thread_pool_wait_stop().
 *
 * If all workers stop before consuming any pending jobs, then those
 * pending jobs will remain pending until the pool is re-enabled via
 * ph_thread_pool_start_workers().
 */
void ph_thread_pool_signal_stop(ph_thread_pool_t *pool);

/** Signal and wait for a thread pool to stop its workers
 *
 * Calls ph_thread_pool_signal_stop() and then joins with all of the
 * worker threads, returning only when the pool has no more remaining
 * workers.
 *
 * This function blocks until there are no more workers.
 *
 * It is undefined what will happen if you call ph_thread_pool_start_workers()
 * before ph_thread_pool_wait_stop() completes.
 */
void ph_thread_pool_wait_stop(ph_thread_pool_t *pool);

/** Cause a thread pool to spin up its workers
 *
 * You almost never need to call this function.  libPhenom will start all
 * defined thread pools as part of the initialization it performs in
 * ph_sched_run().
 *
 * The only time that you might possibly need to call this function is if
 * you have called ph_thread_pool_wait_stop() and later want to restart
 * the pool and spin up its workers.
 *
 * It is undefined what will happen if you call ph_thread_pool_start_workers()
 * before ph_thread_pool_wait_stop() completes.
 */
bool ph_thread_pool_start_workers(ph_thread_pool_t *pool);

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
  // How many items are pending dispatch; includes items
  // in the pool rings and threads attempting to enqueue
  // to the pool
  int64_t num_pending;
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
 * Can be called from any thread. Once the scheduler is stopped, it cannot be restarted. */
void ph_sched_stop(void);

typedef void (*ph_nbio_affine_func)(intptr_t code, void *arg);

/** Queue an affine function dispatch
 *
 * Arranges to call FUNC with the supplied CODE and ARG parameters.
 * The function will be called in the context of the emitter thread
 * with the specified emitter_affinity value.
 *
 * This is useful in situations where you need to serialize the
 * execution of FUNC with respect to IO or timer based function
 * dispatch.
 *
 * Affine functions are dispatched in FIFO order with respect
 * to other affine functions for a given emitter, and are batched together
 * between IO and timer dispatches for that emitter.
 *
 * Note: if your affine function is operating on a job that is scheduled for IO
 * or timer callbacks, it is possible that that job will be dispatched for IO
 * or timer callbacks between the time that your affine function is queued and
 * dispatched.
 *
 * The CODE and ARG parameters are passed through to your affine
 * function callback; their use is entirely up to you.  Note that
 * the CODE parameter can hold an integer or a pointer value.
 *
 * Should you need to pass more than two parameters to the affine
 * function, you will need to allocate storage to hold the information;
 * if you do so, you must ensure that the storage is released at the
 * appropriate time, as the affine function dispatcher does not
 * know how to release it for you.
 *
 * The queue request can fail due to OOM conditions.
 */
ph_result_t ph_nbio_queue_affine_func(uint32_t emitter_affinity,
    ph_nbio_affine_func func, intptr_t code, void *arg);

/** Queue a request to dispatch the job with `PH_IOMASK_WAKEUP`
 *
 * The dispatch will happen as soon as the nbio emitter associated
 * with the job wakes up and processes the request.
 *
 * The ph_job_wakeup request can fail due to OOM conditions or
 * any condition that can cause ph_nbio_queue_affine_func() to fail.
 *
 * Note: it is possible that that job will be dispatched for
 * IO or timer callbacks between the time that ph_job_wakeup() is
 * called and when the `PH_IOMASK_WAKEUP` is dispatched.
 */
ph_result_t ph_job_wakeup(ph_job_t *job);

/** Returns true if any wakeups are pending for this job */
static inline bool ph_job_has_pending_wakeup(ph_job_t *job) {
  return ck_pr_load_32(&job->n_wakeups_pending) > 0;
}

typedef void (*ph_job_collector_func)(ph_thread_t *me);

/** Register a worker collector callback
 *
 * Certain workloads will benefit from aggressive caching or relaxed
 * cleanup processing while the system is busy.  In order to provide
 * timely resource reclamation, an application may register one or
 * more collector callbacks.
 *
 * A collector callback is invoked by emitter threads in the NBIO
 * pool and worker threads in other thread pools when that thread
 * reaches a quiescent state.  For NBIO threads, this is when a given
 * emitter thread has not dispatched a job in the past `$.nbio.max_sleep`
 * milliseconds.  For worker threads, this is when a given thread waits
 * `$.nbio.max_sleep` without being woken up.  These states are assessed
 * per thread.  An idle system will trigger a collector once per thread
 * every `$.nbio.max_sleep` milliseconds while idle.
 *
 * The collector callback is invoked in the context of the thread that
 * is now quiescent and is passed the `ph_thread_t` for that thread.
 *
 * The collector callback should ideally restrict itself to cleaning up data
 * associated with the current thread.
 */
ph_result_t ph_job_collector_register(ph_job_collector_func func);

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
 * The default value for this is `5000` milliseconds; it should be
 * more frequent than your Gimli watchdog interval.  You may disable
 * barrier and heartbeat by setting this option to `0`.
 *
 * `$.nbio.max_sleep` specifies the maximum duration that an nbio
 * or worker thread will be idle.  The default value for this is
 * `5000` milliseconds.  This is important when it comes to handling
 * deferred memory reclamation; after the max sleep expires, and if
 * no events are due, the worker will call ph_thread_epoch_poll()
 * to speculatively reclaim memory.
 */
ph_result_t ph_nbio_init(uint32_t sched_cores);

void _ph_job_set_pool_immediate(ph_job_t *job, ph_thread_t *me);
void _ph_job_pool_start_threads(void);

static inline bool ph_job_have_deferred_items(ph_thread_t *me)
{
  return PH_STAILQ_FIRST(&me->pending_nbio) ||
         PH_STAILQ_FIRST(&me->pending_pool);
}

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


// Copyright 2004-present Facebook. All Rights Reserved

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
    ph_time_t now,
    void *data
);

struct ph_job {
  /** data associated with job */
  void *data;
  ph_job_func_t callback;
  ph_thread_t *owner;
  uint32_t vers;
  uint32_t tvers;

  /** for PH_RUNCLASS_NBIO, trigger mask */
  ph_iomask_t mask;
  ph_socket_t fd;
  ph_time_t   timeout;
  struct ph_timerwheel_timer timer;

  /** desired runclass */
  uint8_t     runclass;

  /** for pooled mode, linkage in the pool queue */
  PH_LIST_ENTRY(ph_job) q_ent;
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

/** Configure a job for NBIO
 */
ph_result_t ph_job_set_nbio(
    ph_job_t *job,
    ph_iomask_t mask,
    ph_time_t timeout);

/** Configure a job to run at a specific time */
ph_result_t ph_job_set_timer_at(
    ph_job_t *job,
    ph_time_t abstime);

/** Configure a job to run after an interval */
ph_result_t ph_job_set_timer_in(
    ph_job_t *job,
    ph_time_t interval);

/** Configure a job for pooled use
 */
ph_result_t ph_job_set_pool(
    ph_job_t *job,
    uint8_t runclass);


/** Attempt to de-queue a job.
 *
 * Attempt to un-do the effect of ph_job_enqueue, for example, to
 * cancel a session.
 */
ph_result_t ph_job_dequeue(ph_job_t *job);

ph_result_t ph_nbio_init(uint32_t sched_cores);
ph_result_t ph_sched_run(void);
void ph_sched_stop(void);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


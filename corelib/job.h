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

#ifndef CORELIB_JOB_H
#define CORELIB_JOB_H

#include "phenom/counter.h"
#include "phenom/sysutil.h"
#include "phenom/variant.h"
#include <ck_ring.h>
#include <ck_spinlock.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __sun__
# include <sys/lwp.h>
#endif

#ifdef __linux__
# define USE_FUTEX 1
#else
# define USE_COND 1
#endif

#ifdef HAVE_KQUEUE
struct ph_nbio_kq_set {
  int size;
  int used;
  struct kevent *events;
  struct kevent base[16];
};
#endif

struct ph_nbio_affine_job {
  PH_STAILQ_ENTRY(ph_nbio_affine_job) ent;
  ph_nbio_affine_func func;
  intptr_t code;
  void *arg;
};
typedef PH_STAILQ_HEAD(affine_ent, ph_nbio_affine_job)
  ph_nbio_affine_job_stailq_t;

struct ph_nbio_emitter {
  ph_timerwheel_t wheel;
  ph_job_t timer_job;
  uint32_t emitter_id;
  struct timeval last_dispatch;
  int io_fd, timer_fd;
  ph_nbio_affine_job_stailq_t affine_jobs;
  ph_job_t affine_job;
  ph_pingfd_t affine_ping;
#ifdef HAVE_PORT_CREATE
  timer_t port_timer;
#endif
  ph_thread_t *thread;
  ph_counter_block_t *cblock;
#ifdef HAVE_KQUEUE
  struct ph_nbio_kq_set kqset;
#endif
};

struct ph_thread_pool_wait {
  uint32_t num_waiting;
  char pad[CK_MD_CACHELINE - sizeof(uint32_t)];
#ifdef USE_FUTEX
  int futcounter;
#elif defined(USE_COND)
  pthread_mutex_t m;
  pthread_cond_t c;
#endif
};

// Bounded by sizeof(used_rings). The last ring is the
// contended ring, so this must be 1 less than the number
// of available bits.  sizeof(used_rings) is in-turn
// bounded by the number of bits supported by the ffs()
// intrinsic
#define MAX_RINGS ((sizeof(intptr_t)*8)-1)

struct ph_thread_pool {
  struct ph_thread_pool_wait consumer CK_CC_CACHELINE;

  uint32_t max_queue_len;

  ck_ring_t rings[MAX_RINGS+1];
  void *buffers[MAX_RINGS+1];
  intptr_t used_rings;

  ck_spinlock_t lock CK_CC_CACHELINE;
  char pad1[CK_MD_CACHELINE - sizeof(ck_spinlock_t)];

  struct ph_thread_pool_wait producer CK_CC_CACHELINE;
  int stop;

  char *name;
  ph_counter_scope_t *counters;
  CK_LIST_ENTRY(ph_thread_pool) plink;
  ph_thread_t **threads;

  uint32_t max_workers;
  uint32_t num_workers;

  ph_variant_t *config;
};


static inline bool has_pending_affine_jobs(struct ph_nbio_emitter *e) {
  return !PH_STAILQ_EMPTY(&e->affine_jobs);
}
void ph_nbio_process_affine_jobs(struct ph_nbio_emitter *e);

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

struct ph_nbio_emitter *ph_nbio_emitter_for_job(ph_job_t *job);
bool ph_thread_set_affinity_policy(ph_thread_t *me, ph_variant_t *policy);

void ph_job_collector_emitter_call(struct ph_nbio_emitter *emitter);
void ph_job_collector_call(ph_thread_t *me);

static inline pid_t get_own_tid(void) {
#if defined(__linux__)
  return syscall(SYS_gettid);
#elif defined(__MACH__)
  uint64_t tid;
  pthread_threadid_np(pthread_self(), &tid);
  return tid;
#elif defined(__sun__)
  return _lwp_self();
#else
  return (pid_t)(intptr_t)&errno;
#endif
}

#endif

/* vim:ts=2:sw=2:et:
 */

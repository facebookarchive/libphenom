/*
 * Copyright 2012-present Facebook, Inc.
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
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/configuration.h"
#include "corelib/job.h"

#ifdef HAVE_KQUEUE

static inline void init_kq_set(struct ph_nbio_kq_set *set)
{
  set->used = 0;
  set->events = set->base;
  set->size = sizeof(set->base) / sizeof(set->base[0]);
}

static void grow_kq_set(struct ph_nbio_kq_set *set)
{
  struct kevent *k;

  if (set->events == set->base) {
    k = malloc(set->size * 2 * sizeof(*k));
    if (!k) {
      ph_panic("OOM");
    }
    memcpy(k, set->events, set->used * sizeof(*k));
    set->events = k;
  } else {
    k = realloc(set->events, set->size * 2 * sizeof(*k));
    if (!k) {
      ph_panic("OOM");
    }
    set->events = k;
  }
  set->size *= 2;
}

static inline void dispose_kq_set(struct ph_nbio_kq_set *set)
{
  if (set->events == set->base) {
    return;
  }
  free(set->events);
}

void ph_nbio_emitter_init(struct ph_nbio_emitter *emitter)
{
  struct kevent tev;

  emitter->io_fd = kqueue();
  if (emitter->io_fd == -1) {
    ph_panic("kqueue(): `Pe%d", errno);
  }
  init_kq_set(&emitter->kqset);

  // Configure timer
  EV_SET(&tev, 0, EVFILT_TIMER, EV_ADD, 0, WHEEL_INTERVAL_MS, emitter);
  if (kevent(emitter->io_fd, &tev, 1, NULL, 0, NULL)) {
    ph_panic("setting up timer: kevent: `Pe%d", errno);
  }
}

static inline void dispatch_kevent(struct ph_nbio_emitter *emitter,
    ph_thread_t *thread, struct kevent *event)
{
  ph_iomask_t mask;
  ph_job_t *job;

  if (event->filter != EVFILT_TIMER && (event->flags & EV_ERROR) != 0) {
    // We're pretty strict about errors at this stage to try to
    // ensure that we're doing the right thing.  There may be
    // cases that we should ignore
    ph_panic("kqueue error on fd:%d `Pe%d",
        (int)event->ident, (int)event->data);
  }

  switch (event->filter) {
    case EVFILT_TIMER:
      gettimeofday(&thread->now, NULL);
      thread->refresh_time = false;
      ph_nbio_emitter_timer_tick(emitter);
      break;

    case EVFILT_READ:
      mask = PH_IOMASK_READ;

      // You'd think that we'd want to do this here, but EV_EOF can
      // be set when we notice that read has been shutdown, but while
      // we still have data in the buffer that we want to read.
      // On this platform we detect EOF as part of attempting to read
      /*
      if (event->flags & EV_EOF) {
        mask |= PH_IOMASK_ERR;
      }
      */

      thread->refresh_time = true;
      job = event->udata;
      job->kmask = 0;
      ph_nbio_emitter_dispatch_immediate(emitter, job, mask);
      break;

    case EVFILT_WRITE:
      thread->refresh_time = true;
      job = event->udata;
      job->kmask = 0;
      ph_nbio_emitter_dispatch_immediate(emitter, job, PH_IOMASK_WRITE);
      break;
  }
}

void ph_nbio_emitter_run(struct ph_nbio_emitter *emitter, ph_thread_t *thread)
{
  int n, i;
  int max_chunk, max_sleep;
  struct timespec ts;

  max_chunk = ph_config_query_int("$.nbio.max_per_wakeup", 1024);
  max_sleep = ph_config_query_int("$.nbio.max_sleep", 5000);
  ts.tv_sec = max_sleep / 1000;
  ts.tv_nsec = (max_sleep - (ts.tv_sec * 1000)) * 1000000;

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = kevent(emitter->io_fd, emitter->kqset.events, emitter->kqset.used,
          emitter->kqset.events, MIN(emitter->kqset.size, max_chunk), &ts);

    if (n < 0 && errno != EINTR) {
      ph_panic("kevent: `Pe%d", errno);
    }

    if (n <= 0) {
      ph_job_collector_emitter_call(emitter);
      ph_thread_epoch_poll();
      continue;
    }

    ph_thread_epoch_begin();
    for (i = 0; i < n; i++) {
      dispatch_kevent(emitter, thread, &emitter->kqset.events[i]);
    }

    if (n + 1 >= emitter->kqset.size) {
      grow_kq_set(&emitter->kqset);
    }
    emitter->kqset.used = 0;

    if (ph_job_have_deferred_items(thread)) {
      ph_job_pool_apply_deferred_items(thread);
    }
    ph_thread_epoch_end();
    ph_job_collector_emitter_call(emitter);
    ph_thread_epoch_poll();
  }

  dispose_kq_set(&emitter->kqset);
}

ph_result_t ph_nbio_emitter_apply_io_mask(struct ph_nbio_emitter *emitter,
    ph_job_t *job, ph_iomask_t mask)
{
  struct kevent kev[2];
  int nev = 0;
  int res;

  if (job->fd == -1) {
    return PH_OK;
  }

  if (mask & PH_IOMASK_READ) {
    EV_SET(&kev[nev], job->fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, job);
    nev++;
  }
  if (mask & PH_IOMASK_WRITE) {
    EV_SET(&kev[nev], job->fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, job);
    nev++;
  }
  if ((mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) == 0) {
    // Neither read nor write -> delete
    EV_SET(&kev[nev], job->fd, EVFILT_READ, EV_DELETE, 0, 0, job);
    nev++;
    EV_SET(&kev[nev], job->fd, EVFILT_WRITE, EV_DELETE, 0, 0, job);
    nev++;
  }

  job->kmask = mask;
  job->mask = mask;

  res = kevent(emitter->io_fd, kev, nev, NULL, 0, NULL);
  if (res != 0 && mask == 0 && errno == ENOENT) {
    // It's "OK" if we decided to delete it and it wasn't there
    res = 0;
  }
  if (res != 0) {
    ph_panic("kevent: setting mask to %02x on fd %d with %d slots -> `Pe%d",
        mask, job->fd, nev, errno);
    return PH_ERR;
  }
  return PH_OK;
}

ph_iomask_t ph_job_get_kmask(ph_job_t *job)
{
  return job->kmask;
}

#endif

/* vim:ts=2:sw=2:et:
 */


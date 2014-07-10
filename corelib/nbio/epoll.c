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
#include "phenom/configuration.h"
#include "corelib/job.h"

#ifdef HAVE_EPOLL_CREATE
#define DEFAULT_POLL_MASK EPOLLHUP|EPOLLERR|EPOLLONESHOT

static void tick_epoll(ph_job_t *job, ph_iomask_t why, void *data)
{
  uint64_t expirations = 0;
  struct ph_nbio_emitter *emitter = data;

  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  /* consume the number of ticks; ideally this is 1; anything bigger
   * means that we've fallen behind */
  if (read(emitter->timer_fd, &expirations, sizeof(expirations)) > 0) {
    if (expirations) {
      ph_nbio_emitter_timer_tick(emitter);
    }
  }

  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

#ifndef TFD_CLOEXEC
# define TFD_CLOEXEC 02000000
#endif
#ifndef TFD_NONBLOCK
# define TFD_NONBLOCK 04000
#endif

#ifndef HAVE_TIMERFD_CREATE
# include <sys/syscall.h>

#ifndef SYS_timerfd_create
# define SYS_timerfd_create 283
#endif
#ifndef SYS_timerfd_settime
# define SYS_timerfd_settime 286
#endif

static int timerfd_create(int clockid, int flags)
{
  return syscall(SYS_timerfd_create, clockid, flags);
}

static int timerfd_settime(int fd, int flags,
  const struct itimerspec *new_value,
  struct itimerspec *old_value)
{
  return syscall(SYS_timerfd_settime, fd, flags, new_value, old_value);
}
#endif

void ph_nbio_emitter_init(struct ph_nbio_emitter *emitter)
{
  struct itimerspec ts;

#ifdef HAVE_EPOLL_CREATE1
  emitter->io_fd = epoll_create1(EPOLL_CLOEXEC);
#else
  emitter->io_fd = epoll_create(1024*1024);
#endif

  if (emitter->io_fd == -1) {
    ph_panic("epoll_create: `Pe%d", errno);
  }

#ifndef HAVE_EPOLL_CREATE1
  fcntl(emitter->io_fd, F_SETFD, FD_CLOEXEC);
#endif

  emitter->timer_fd = timerfd_create(
      CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
  if (emitter->timer_fd == -1) {
    ph_panic("timerfd_create(CLOCK_MONOTONIC) failed: `Pe%d", errno);
  }

  memset(&ts, 0, sizeof(ts));
  ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
  ts.it_value.tv_nsec = ts.it_interval.tv_nsec;
  timerfd_settime(emitter->timer_fd, 0, &ts, NULL);

  ph_job_init(&emitter->timer_job);
  emitter->timer_job.callback = tick_epoll;
  emitter->timer_job.fd = emitter->timer_fd;
  emitter->timer_job.data = emitter;
  emitter->timer_job.emitter_affinity = emitter->emitter_id;
  ph_job_set_nbio(&emitter->timer_job, PH_IOMASK_READ, 0);
}

void ph_nbio_emitter_run(struct ph_nbio_emitter *emitter, ph_thread_t *thread)
{
  struct epoll_event *event;
  int n, i;
  int max_chunk, max_sleep;

  max_chunk = ph_config_query_int("$.nbio.max_per_wakeup", 1024);
  max_sleep = ph_config_query_int("$.nbio.max_sleep", 5000);
  event = malloc(max_chunk * sizeof(struct epoll_event));

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = epoll_wait(emitter->io_fd, event, max_chunk, max_sleep);
    thread->refresh_time = true;

    if (n < 0) {
      if (errno != EINTR) {
        ph_log(PH_LOG_ERR, "epoll_wait: `Pe%d", errno);
      }
      ph_job_collector_emitter_call(emitter);
      ph_thread_epoch_poll();
      continue;
    }

    if (n == 0) {
      continue;
    }

    ph_thread_epoch_begin();
    for (i = 0; i < n; i++) {
      ph_iomask_t mask = 0;
      ph_job_t *job = event[i].data.ptr;

      if (job->mask == 0) {
        // Ignore: disabled for now
        continue;
      }

      switch (event[i].events & (EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
        case EPOLLIN:
          mask = PH_IOMASK_READ;
          break;
        case EPOLLOUT:
          mask = PH_IOMASK_WRITE;
          break;
        case EPOLLIN|EPOLLOUT:
          mask = PH_IOMASK_READ|PH_IOMASK_WRITE;
          break;
        default:
          mask = PH_IOMASK_ERR;
      }
      // We can't just clear kmask completely because ONESHOT retains
      // the existence of the item; we need to know it is there so that
      // we can MOD it instead of ADD it later.
      job->kmask = DEFAULT_POLL_MASK;
      ph_nbio_emitter_dispatch_immediate(emitter, job, mask);
      if (ph_job_have_deferred_items(thread)) {
        ph_job_pool_apply_deferred_items(thread);
      }
    }
    ph_thread_epoch_end();
    ph_job_collector_emitter_call(emitter);
    ph_thread_epoch_poll();
  }

  free(event);
}

ph_result_t ph_nbio_emitter_apply_io_mask(
    struct ph_nbio_emitter *emitter, ph_job_t *job, ph_iomask_t mask)
{
  struct epoll_event evt;
  int res;
  int want_mask;

  if (job->fd == -1) {
    return PH_OK;
  }

  switch (mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) {
    case PH_IOMASK_READ|PH_IOMASK_WRITE:
      want_mask = EPOLLIN|EPOLLOUT|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_READ:
      want_mask = EPOLLIN|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_WRITE:
      want_mask = EPOLLOUT|DEFAULT_POLL_MASK;
      break;
    case 0:
    default:
      want_mask = 0;
      break;
  }

  if (want_mask == 0) {
    job->mask = 0;
    job->kmask = 0;
    res = epoll_ctl(emitter->io_fd, EPOLL_CTL_DEL, job->fd, &evt);
    if (res < 0 && errno == ENOENT) {
      res = 0;
    }
  } else {
    int op = job->kmask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    evt.events = want_mask;
    evt.data.ptr = job;

    // Set the masks on the job before we epoll_ctl as it
    // may arrive at another thread *before* epoll_ctl returns
    job->kmask = want_mask;
    job->mask = mask;
    res = epoll_ctl(emitter->io_fd, op, job->fd, &evt);
    if (res == -1 && errno == EEXIST && op == EPOLL_CTL_ADD) {
      // This can happen when we're transitioning between distinct job
      // pointers, for instance, when we're moving from an async connect
      // to setting up the sock job
      res = epoll_ctl(emitter->io_fd, EPOLL_CTL_MOD, job->fd, &evt);
    } else if (res == -1 && errno == ENOENT && op == EPOLL_CTL_MOD) {
      res = epoll_ctl(emitter->io_fd, EPOLL_CTL_ADD, job->fd, &evt);
    }

    if (res == -1 && errno == EEXIST) {
      res = 0;
    }
  }

  if (res == -1) {
    ph_panic(
        "fd=%d (callback=%p) epoll_ctl: setting mask to %02x -> %d `Pe%d",
        job->fd, (void*)(uintptr_t)job->callback, mask, errno, errno);
    ph_log_stacktrace(PH_LOG_ERR);
    return PH_ERR;
  }
  return PH_OK;
}

ph_iomask_t ph_job_get_kmask(ph_job_t *job)
{
  switch (job->kmask & (EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
    case EPOLLIN:
      return PH_IOMASK_READ;
    case EPOLLOUT:
      return PH_IOMASK_WRITE;
    case EPOLLIN|EPOLLOUT:
      return PH_IOMASK_READ|PH_IOMASK_WRITE;
    default:
      return 0;
  }
}

#endif

/* vim:ts=2:sw=2:et:
 */


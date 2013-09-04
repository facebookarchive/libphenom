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

#include "phenom/socket.h"
#include "phenom/memory.h"
#include "phenom/job.h"

struct connect_job {
  ph_job_t job;
  ph_socket_t s;
  ph_sockaddr_t addr;
  int status;
  struct timeval start;
  void *arg;
  ph_socket_connect_func func;
};

static ph_memtype_def_t def = {
  "socket", "connect_job", sizeof(struct connect_job), PH_MEM_FLAGS_ZERO
};
static ph_memtype_t mt_connect_job;
static pthread_once_t done_sock_init = PTHREAD_ONCE_INIT;

static void do_sock_init(void)
{
  mt_connect_job = ph_memtype_register(&def);
}

ph_socket_t ph_socket_for_addr(ph_sockaddr_t *addr, int type, int flags)
{
  ph_socket_t s;

  s = socket(addr->family, type, 0);
  if (s == -1) {
    return s;
  }

  if (flags & PH_SOCK_CLOEXEC) {
    fcntl(s, F_SETFD, FD_CLOEXEC);
  }

  if (flags & PH_SOCK_NONBLOCK) {
    ph_socket_set_nonblock(s, true);
  }

  return s;
}

static void connect_complete(ph_job_t *j, ph_iomask_t why, void *data)
{
  struct connect_job *job = data;
  int status = 0;
  struct timeval done;

  unused_parameter(j);

  if (why == PH_IOMASK_TIME) {
    // Timed out waiting for connect
    status = ETIMEDOUT;
  } else {
    // Check status
    socklen_t slen = sizeof(status);

    if (getsockopt(job->s, SOL_SOCKET, SO_ERROR, (void*)&status, &slen) < 0) {
      status = errno;
    }
  }

  done = ph_time_now();
  timersub(&done, &job->start, &done);
  job->func(job->s, &job->addr, status, &done, job->arg);
  ph_mem_free(mt_connect_job, job);
}

void ph_socket_connect(ph_socket_t s, const ph_sockaddr_t *addr,
  struct timeval *timeout, ph_socket_connect_func func, void *arg)
{
  struct connect_job *job;
  int res;
  struct timeval default_timeout = { 60, 0 };
  struct timeval done = { 0, 0 };

  pthread_once(&done_sock_init, do_sock_init);

  job = ph_mem_alloc(mt_connect_job);
  if (!job) {
    func(s, addr, ENOMEM, &done, arg);
    return;
  }

  if (ph_job_init(&job->job) != PH_OK) {
    func(s, addr, ENOMEM, &done, arg);
    return;
  }

  job->s = s;
  job->addr = *addr;
  job->func = func;
  job->arg = arg;

  job->start = ph_time_now();
  res = connect(s, &job->addr.sa.sa, ph_sockaddr_socklen(&job->addr));

  if (res < 0 && errno == EINPROGRESS) {
    // Pending; let's set things up to call us back later

    job->job.callback = connect_complete;
    job->job.data = job;
    ph_job_set_nbio_timeout_in(&job->job, PH_IOMASK_WRITE,
        timeout ? *timeout : default_timeout);
    return;
  }

  done = ph_time_now();
  timersub(&done, &job->start, &done);

  // Immediate result
  func(s, addr, res == 0 ? 0 : errno, &done, arg);
  ph_mem_free(mt_connect_job, job);
}

/* vim:ts=2:sw=2:et:
 */


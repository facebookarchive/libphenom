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

#include "phenom/sysutil.h"
#include "phenom/log.h"

/* We create the pingfd using non-blocking descriptors.
 * The rationale is that if the fd is too busy to take on
 * more pings then the receiving side will be actively
 * consuming work from whatever queue is associated with it,
 * so it won't matter if we "lose" notifications.
 * The flip side is that the aggressive "stealing" of work
 * will mean that some workers will observe more spurious
 * wakeups when someone has stolen from the queue */

ph_result_t ph_pingfd_init(ph_pingfd_t *pfd)
{
#ifdef HAVE_EVENTFD
  pfd->fds[0] = eventfd(0, EFD_CLOEXEC|EFD_SEMAPHORE|EFD_NONBLOCK);

  if (pfd->fds[0] == -1) {
    ph_log(PH_LOG_ERR, "pingfd_init: eventfd `Pe%d", errno);
    return PH_ERR;
  }
#else
  if (ph_pipe(pfd->fds, PH_PIPE_NONBLOCK|PH_PIPE_CLOEXEC)) {
    ph_log(PH_LOG_ERR, "pingfd_init: pipe `Pe%d", errno);
    return PH_ERR;
  }
#endif

  return PH_OK;
}

ph_result_t ph_pingfd_ping(ph_pingfd_t *pfd)
{
  int res;
  while (true) {
#ifdef HAVE_EVENTFD
    if (eventfd_write(pfds->fds[0], 1) == 0) {
      return PH_OK;
    }
#else
    res = write(pfd->fds[1], "p", 1);
    if (res == 1) {
      return PH_OK;
    }
#endif

    if (errno == EAGAIN) {
      return PH_OK;
    }

    if (errno == EINTR) {
      continue;
    }
    ph_log(PH_LOG_ERR, "pingfd_ping `Pe%d", errno);
    return PH_ERR;
  }
}

ph_result_t ph_pingfd_close(ph_pingfd_t *pfd)
{
  close(pfd->fds[0]);
  pfd->fds[0] = -1;
#ifndef HAVE_EVENTFD
  close(pfd->fds[1]);
  pfd->fds[1] = -1;
#endif

  return PH_OK;
}

ph_socket_t ph_pingfd_get_fd(ph_pingfd_t *pfd)
{
  return pfd->fds[0];
}

uint32_t ph_pingfd_consume_all(ph_pingfd_t *pfd)
{
  uint32_t res = 0;
#ifdef HAVE_EVENTFD
  while (ph_pingfd_consume_one(pfd)) {
    res++;
  }
  return res;
#else
  int n;
  char buf[64];

  do {
    n = read(pfd->fds[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    res += n;
  } while (n == sizeof(buf));
  return res;
#endif
}

bool ph_pingfd_consume_one(ph_pingfd_t *pfd)
{
#ifdef HAVE_EVENTFD
  eventfd_t value;

  if (eventfd_read(pfd->fds[0], &value) == 0) {
    return true;
  }
  return false;
#else
  char buf[1];

  if (read(pfd->fds[0], buf, sizeof(buf)) == 1) {
    return true;
  }
  return false;
#endif
}


/* vim:ts=2:sw=2:et:
 */


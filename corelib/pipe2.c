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

ph_result_t ph_pipe(ph_socket_t fds[2], int flags)
{
#ifdef HAVE_PIPE2
  int pflags = 0;

  if (flags & PH_PIPE_NONBLOCK) {
    pflags |= O_NONBLOCK;
  }
  if (flags & PH_PIPE_CLOEXEC) {
    pflags |= O_CLOEXEC;
  }

  return pipe2(fds, pflags);
#else
  int res;

  res = pipe(fds);
  if (res != 0) {
    return res;
  }

  if (flags == 0) {
    return 0;
  }

  if (flags & PH_PIPE_CLOEXEC) {
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  }
  if (flags & PH_PIPE_NONBLOCK) {
    ph_socket_set_nonblock(fds[0], true);
    ph_socket_set_nonblock(fds[1], true);
  }

  return 0;
#endif
}


/* vim:ts=2:sw=2:et:
 */


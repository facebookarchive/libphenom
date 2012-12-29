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

#ifndef HAVE_PIPE2
int pipe2(int pipefd[2], int flags)
{
  int res;

  res = pipe(pipefd);
  if (res != 0) {
    return res;
  }

  if (flags == 0) {
    return 0;
  }

  if (flags & O_CLOEXEC) {
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
  }
  if (flags & O_NONBLOCK) {
    phenom_socket_set_nonblock(pipefd[0], true);
    phenom_socket_set_nonblock(pipefd[1], true);
  }

  return 0;
}
#endif


/* vim:ts=2:sw=2:et:
 */


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
#include "phenom/sysutil.h"
#include "phenom/stream.h"

static bool should_retry(ph_stream_t *stm)
{
  switch (ph_stm_errno(stm)) {
    case EAGAIN:
    case EINTR:
    case EINPROGRESS:
      return true;
    default:
      return false;
  }
}

static bool fd_close(ph_stream_t *stm)
{
  int fd = (intptr_t)stm->cookie;

  if (close(fd) == 0) {
    return true;
  }
  stm->last_err = errno;
  return false;
}

static bool fd_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  int fd = (intptr_t)stm->cookie;
  int r;

  r = readv(fd, iov, iovcnt);

  if (r == -1) {
    stm->last_err = errno;
    if (should_retry(stm)) {
      stm->need_mask |= PH_IOMASK_READ;
    }
    return false;
  }

  if (nread) {
    *nread = r;
  }
  return true;
}

static bool fd_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrote)
{
  int fd = (intptr_t)stm->cookie;
  int w;

  w = writev(fd, iov, iovcnt);

  if (w == -1) {
    stm->last_err = errno;
    if (should_retry(stm)) {
      stm->need_mask |= PH_IOMASK_WRITE;
    }
    return false;
  }

  if (nwrote) {
    *nwrote = w;
  }
  return true;
}

static bool fd_seek(ph_stream_t *stm, int64_t delta,
    int whence, uint64_t *newpos)
{
  int fd = (intptr_t)stm->cookie;
  off_t off;

  off = lseek(fd, delta, whence);

  if (off == (off_t)-1) {
    stm->last_err = errno;
    return false;
  }

  if (newpos) {
    *newpos = off;
  }
  return true;
}

struct ph_stream_funcs ph_stm_funcs_fd = {
  fd_close,
  fd_readv,
  fd_writev,
  fd_seek
};

ph_stream_t *ph_stm_fd_open(int fd, int sflags, uint32_t bufsize)
{
  return ph_stm_make(&ph_stm_funcs_fd, (void*)(intptr_t)fd, sflags, bufsize);
}

ph_stream_t *ph_stm_file_open(const char *filename, int oflags, int mode)
{
  int fd;
  ph_stream_t *stm;

#ifdef O_LARGEFILE
  oflags |= O_LARGEFILE;
#endif

  fd = open(filename, oflags, mode);
  if (fd < 0) {
    return NULL;
  }

  stm = ph_stm_fd_open(fd, 0, PH_STM_BUFSIZE);
  if (!stm) {
    close(fd);
    errno = ENOMEM;
  }

  return stm;
}

/* vim:ts=2:sw=2:et:
 */


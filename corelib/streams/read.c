/*
Copyright 2005-2013 Rich Felker
Copyright 2013 Facebook, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/stream.h"
#include "phenom/log.h"

bool ph_stm_readahead(ph_stream_t *stm, uint64_t count)
{
  if (!ph_stm_flush(stm)) {
    return false;
  }

  if (!stm->bufsize) {
    // Sure, there's no buffer, so we didn't encounter a problem
    // filling it
    return true;
  }

  ph_stm_lock(stm);

  if ((uint64_t)(stm->rend - stm->rpos) >= count) {
    // Don't need to read anything
    ph_stm_unlock(stm);
    return true;
  }

  // How much room is in the buffer
  uint64_t bufavail;

  if (stm->rend) {
    unsigned char *bufend = stm->buf + stm->bufsize;
    bufavail = bufend - stm->rend;
  } else {
    stm->rpos = stm->buf;
    stm->rend = stm->buf;
    bufavail = stm->bufsize;
  }

  if (!bufavail) {
    // Full
    ph_stm_unlock(stm);
    return true;
  }

  // Fill up the remaining buffer space
  struct iovec vec = {
    .iov_base = stm->rend,
    .iov_len = bufavail
  };

  uint64_t nread;
  if (!stm->funcs->readv(stm, &vec, 1, &nread)) {
    ph_stm_unlock(stm);
    errno = ph_stm_errno(stm);
    return false;
  }
  stm->rend += nread;

  ph_stm_unlock(stm);
  return true;
}

bool ph_stm_read(ph_stream_t *stm, void *buf, uint64_t count, uint64_t *nread)
{
  int64_t res = 0;
  int64_t x;
  unsigned char *dest = (unsigned char*)buf;

  // Flush any pending writes
  if (!ph_stm_flush(stm)) {
    return false;
  }

  ph_stm_lock(stm);
  stm->last_err = 0;

  if (count == 0) {
    if (nread) {
      *nread = 0;
    }
    ph_stm_unlock(stm);
    return true;
  }

  if (stm->rend - stm->rpos > 0) {
    // First consume the buffer
    x = MIN((uint64_t)(stm->rend - stm->rpos), count);
    memcpy(dest, stm->rpos, x);
    stm->rpos += x;
    dest += x;
    count -= x;
    res += x;
  }

  while (count > 0) {
    // Read into the dest buffer, but overflow any excess into
    // our buffer.  Handle edge case with readv as discussed in
    // http://git.musl-libc.org/cgit/musl/commit/src/stdio/__stdio_read.c?id=
    //   2cff36a84f268c09f4c9dc5a1340652c8e298dc0
    struct iovec iov[2] = {
      { .iov_base = dest,     .iov_len = count - !!stm->bufsize },
      { .iov_base = stm->buf, .iov_len = stm->bufsize }
    };
    uint64_t cnt;

    if (!stm->funcs->readv(stm, iov, 2, &cnt) || cnt == 0) {
      stm->rpos = stm->rend = 0;

      if (res) {
        // partial success; fall out and report what we read
        break;
      }
      errno = ph_stm_errno(stm);
      ph_stm_unlock(stm);
      return false;
    }

    if (cnt > iov[0].iov_len) {
      cnt -= iov[0].iov_len;

      // We put stuff in the buffer, fix up pointers
      stm->rpos = stm->buf;
      stm->rend = stm->buf + cnt;

      // and handle the edge case:
      if (stm->bufsize) {
        dest[count-1] = *stm->rpos++;
      }
      // we satisfied their read, and them some: don't report
      // more than they asked for
      cnt = count;
    }

    res += cnt;
    dest += cnt;
    count -= cnt;
  }

  if (nread) {
    *nread = res;
  }
  ph_stm_unlock(stm);
  return true;
}

/* vim:ts=2:sw=2:et:
 */


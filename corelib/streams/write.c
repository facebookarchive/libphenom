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
#include "phenom/printf.h"

static bool do_write(ph_stream_t *stm, const void *buf,
    uint64_t count, uint64_t *nwrote)
{
  uint64_t x;
  struct iovec iovs[2];
  struct iovec *iov = iovs;
  int iovcnt = 2;
  uint64_t towrite;

  // First, try to accumulate in our buffer.  Clear any read data
  // that might be in there, so we can re-use the buffer space
  if (!stm->wend) {
    stm->rpos = stm->rend = 0;
    stm->wpos = stm->wbase = stm->buf;
    stm->wend = stm->buf + stm->bufsize;
  }

  if (count && count < (uint64_t)(stm->wend - stm->wpos)) {
    // It fits in the buffer
    memcpy(stm->wpos, buf, count);
    stm->wpos += count;
    if (nwrote) {
      *nwrote = count;
    }
    return true;
  }

  // Doesn't fit, we'll need to write.  Send out anything we might
  // have buffered, followed by the data they're sending now.
  iov[0].iov_base = stm->wbase;
  iov[0].iov_len  = stm->wpos - stm->wbase;
  iov[1].iov_base = (void*)buf;
  iov[1].iov_len  = count;

  towrite = iov[0].iov_len + iov[1].iov_len;
  for (;;) {
    if (!stm->funcs->writev(stm, iov, iovcnt, &x)) {
      stm->wpos = stm->wbase = stm->wend = 0;
      if (iovcnt == 2) {
        errno = ph_stm_errno(stm);
        return false;
      }
      if (nwrote) {
        *nwrote = count - iov[0].iov_len;
      }
      return true;
    }

    if (x == towrite) {
      // We sent everything, there is nothing left to buffer
      stm->wpos = stm->wbase = stm->buf;
      stm->wend = stm->buf + stm->bufsize;
      if (nwrote) {
        *nwrote = count;
      }
      return true;
    }

    // Otherwise we had a partial write
    towrite -= x;
    if (x > iov[0].iov_len) {
      // Consumed existing buffer
      stm->wpos = stm->wbase = stm->buf;
      x -= iov[0].iov_len;
      iov++;
      iovcnt--;
    } else if (iovcnt == 2) {
      stm->wbase += x;
    }

    iov[0].iov_base = (char*)iov[0].iov_base + x;
    iov[0].iov_len -= x;
  }
}

bool ph_stm_flush(ph_stream_t *stm)
{
  ph_stm_lock(stm);

  // Flush the write buffer
  if (stm->wpos > stm->wbase) {
    do_write(stm, 0, 0, 0);
    if (!stm->wpos) {
      // Error during flush
      ph_stm_unlock(stm);
      errno = ph_stm_errno(stm);
      return false;
    }
  }

  ph_stm_unlock(stm);
  return true;
}

static bool stm_print(void *arg, const char *buf, size_t len)
{
  uint64_t wrote;
  return do_write(arg, buf, len, &wrote) && wrote == len;
}

static bool stm_flush(void *arg)
{
  ph_unused_parameter(arg);
  // We don't need to (and shouldn't) flush here; this is just telling
  // us that we could do it.
  return true;
}

static struct ph_vprintf_funcs stm_funcs = {
  stm_print,
  stm_flush
};

int ph_stm_vprintf(ph_stream_t *stm, const char *fmt, va_list ap)
{
  int ret;

  ph_stm_lock(stm);
  stm->last_err = 0;
  ret = ph_vprintf_core(stm, &stm_funcs, fmt, ap);
  ph_stm_unlock(stm);
  return ret;
}

int ph_stm_printf(ph_stream_t *stm, const char *fmt, ...)
{
  int ret;
  va_list ap;

  va_start(ap, fmt);
  ret = ph_stm_vprintf(stm, fmt, ap);
  va_end(ap);

  return ret;
}

bool ph_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrote)
{
  bool res = true;

  ph_stm_lock(stm);
  stm->last_err = 0;

  if (!stm->bufsize) {
    // If unbuffered, pass it through
    res = stm->funcs->writev(stm, iov, iovcnt, nwrote);
  } else {
    int i;
    uint64_t total = 0, n;

    for (i = 0; i < iovcnt; i++) {
      res = do_write(stm, iov[i].iov_base, iov[i].iov_len, &n);
      if (res) {
        total += n;
      } else {
        break;
      }
    }

    if (total) {
      if (nwrote) {
        *nwrote = total;
      }
      res = true;
    }
  }

  ph_stm_unlock(stm);
  return res;
}

bool ph_stm_write(ph_stream_t *stm, const void *buf,
    uint64_t count, uint64_t *nwrote)
{
  bool res;

  ph_stm_lock(stm);
  stm->last_err = 0;
  if (count == 0) {
    *nwrote = 0;
    ph_stm_unlock(stm);
    return true;
  }
  res = do_write(stm, buf, count, nwrote);

  ph_stm_unlock(stm);
  return res;
}

bool ph_stm_seek(ph_stream_t *stm, int64_t delta, int whence, uint64_t *newpos)
{
  if (!ph_stm_flush(stm)) {
    return false;
  }

  ph_stm_lock(stm);
  stm->last_err = 0;
  // Adjust for read buffer
  if (whence == SEEK_CUR) {
    delta -= stm->rend - stm->rpos;
  }

  // Leave write mode
  stm->wpos = stm->wbase = stm->wend = 0;

  if (!stm->funcs->seek(stm, delta, whence, newpos)) {
    errno = ph_stm_errno(stm);
    ph_stm_unlock(stm);
    return false;
  }

  // Discard read buffer (we moved)
  stm->rpos = stm->rend = 0;
  ph_stm_unlock(stm);

  return true;
}

/* vim:ts=2:sw=2:et:
 */


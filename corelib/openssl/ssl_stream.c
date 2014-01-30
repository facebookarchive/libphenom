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

#include "phenom/stream.h"
#include "phenom/openssl.h"
#include "phenom/log.h"
#include <openssl/bio.h>

/* Implements a phenom stream that delegates to an OpenSSL SSL object */

static bool ssl_stm_close(ph_stream_t *stm)
{
  SSL *s = stm->cookie;

  if (s) {
    SSL_free(s);
    stm->cookie = NULL;
  }

  return true;
}

static bool do_ssl_read_or_write(ph_stream_t *stm, bool is_reading,
    const struct iovec *iov, int iovcnt, uint64_t *nread)
{
  int i;
  int res, err;
  unsigned long serr; // NOLINT(runtime/int)
  const char *file;
  int line;
  SSL *s = stm->cookie;
  uint64_t total_read = 0;

  if (ph_unlikely(s == NULL)) {
    stm->last_err = EBADF;
    return false;
  }

  stm->need_mask = 0;

  for (i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }

    if (is_reading) {
      res = SSL_read(s, iov[i].iov_base, iov[i].iov_len);
    } else {
      res = SSL_write(s, iov[i].iov_base, iov[i].iov_len);
    }

    if (res > 0) {
      total_read += res;
      continue;
    }
    err = SSL_get_error(s, res);

    switch (err) {
      case SSL_ERROR_WANT_READ:
        stm->need_mask |= PH_IOMASK_READ;
        stm->last_err = EAGAIN;
        break;

      case SSL_ERROR_WANT_WRITE:
        stm->need_mask |= PH_IOMASK_WRITE;
        stm->last_err = EAGAIN;
        break;

      default:
        stm->last_err = EIO;
        // TODO(wez): maybe capture and track the error stack?
        // We must consume it here regardless
        while ((serr = ERR_get_error_line(&file, &line)) != 0) {
          char ebuf[120];
          ERR_error_string_n(serr, ebuf, sizeof(ebuf));
          ph_log(PH_LOG_INFO, "SSL err: %s:%d %lu %s", file, line, serr, ebuf);
        }
    }
    break;
  }

  if (total_read > 0) {
    if (nread) {
      *nread = total_read;
    }
    return true;
  }

  return false;
}

static bool ssl_stm_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  return do_ssl_read_or_write(stm, true, iov, iovcnt, nread);
}

static bool ssl_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrote)
{
  return do_ssl_read_or_write(stm, false, iov, iovcnt, nwrote);
}

static bool ssl_stm_seek(ph_stream_t *stm, int64_t delta,
    int whence, uint64_t *newpos)
{
  ph_unused_parameter(delta);
  ph_unused_parameter(whence);
  ph_unused_parameter(newpos);
  stm->last_err = ESPIPE;
  return false;
}

static struct ph_stream_funcs ssl_stm_funcs = {
  ssl_stm_close,
  ssl_stm_readv,
  ssl_stm_writev,
  ssl_stm_seek
};

ph_stream_t *ph_stm_ssl_open(SSL *ssl)
{
  return ph_stm_make(&ssl_stm_funcs, ssl, 0, 0);
}

/* vim:ts=2:sw=2:et:
 */


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
#include <openssl/bio.h>

/* Implements an OpenSSL BIO that writes to a phenom bufq */

static int bio_bufq_write(BIO *h, const char *buf, int size)
{
  uint64_t n;
  ph_bufq_t *q = h->ptr;

  BIO_clear_retry_flags(h);
  if (ph_bufq_append(q, buf, size, &n) != PH_OK) {
    BIO_set_retry_write(h);
    errno = EAGAIN;
    return -1;
  }

  return (int)n;
}

static int bio_bufq_puts(BIO *h, const char *str)
{
  return bio_bufq_write(h, str, strlen(str));
}

static int bio_bufq_read(BIO *h, char *buf, int size)
{
  ph_unused_parameter(h);
  ph_unused_parameter(buf);
  ph_unused_parameter(size);
  errno = ENOSYS;
  return -1;
}

static long bio_bufq_ctrl(BIO *h, int cmd, // NOLINT(runtime/int)
    long arg1, void *arg2)                 // NOLINT(runtime/int)
{
  ph_unused_parameter(h);
  ph_unused_parameter(cmd);
  ph_unused_parameter(arg1);
  ph_unused_parameter(arg2);
  return 1;
}

static int bio_bufq_new(BIO *h)
{
  h->init = 0;
  h->num = 0;
  h->ptr = NULL;
  h->flags = 0;
  return 1;
}

static int bio_bufq_free(BIO *h)
{
  if (!h) {
    return 0;
  }

  h->ptr = NULL;
  h->init = 0;
  h->flags = 0;

  return 1;
}

static BIO_METHOD method_bufq = {
  // See bio_stream.c
  81 | BIO_TYPE_SOURCE_SINK,
  "phenom-bufq",
  bio_bufq_write,
  bio_bufq_read,
  bio_bufq_puts,
  NULL, /* no gets */
  bio_bufq_ctrl,
  bio_bufq_new,
  bio_bufq_free,
  NULL, /* no callback ctrl */
};

BIO *ph_openssl_bio_wrap_bufq(ph_bufq_t *bufq)
{
  BIO *h;

  h = BIO_new(&method_bufq);
  if (!h) {
    return NULL;
  }

  h->ptr = bufq;
  h->init = 1;
  return h;
}


/* vim:ts=2:sw=2:et:
 */


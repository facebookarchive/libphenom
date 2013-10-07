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

#include "phenom/sysutil.h"
#include "phenom/thread.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/stream.h"
#include "phenom/openssl.h"

struct CRYPTO_dynlock_value {
  pthread_rwlock_t lock;
};

static ph_memtype_t mt_dynlock;
static struct ph_memtype_def dynlock_def = {
  "crypto", "dynlock", sizeof(struct CRYPTO_dynlock_value), 0
};

static struct CRYPTO_dynlock_value **crypto_locks;

static struct CRYPTO_dynlock_value *crypto_dynlock_new(
    const char *file, int line)
{
  struct CRYPTO_dynlock_value *l;
  int err;

  l = ph_mem_alloc(mt_dynlock);
  if (!l) {
    return NULL;
  }

  err = pthread_rwlock_init(&l->lock, NULL);
  if (err) {
    ph_log(PH_LOG_ERR, "crypto_dynlock_new: mutex_init failed: `Pe%d (%s:%d)",
        err, file, line);
    ph_mem_free(mt_dynlock, l);
    l = NULL;
  }
  return l;
}

static void crypto_dynlock_lock(int mode, struct CRYPTO_dynlock_value *l,
    const char *file, int line)
{
  switch (mode) {
    case CRYPTO_LOCK|CRYPTO_READ:
      pthread_rwlock_rdlock(&l->lock);
      break;

    case CRYPTO_LOCK|CRYPTO_WRITE:
      pthread_rwlock_wrlock(&l->lock);
      break;

    case CRYPTO_UNLOCK|CRYPTO_READ:
    case CRYPTO_UNLOCK|CRYPTO_WRITE:
      pthread_rwlock_unlock(&l->lock);
      break;

    default:
      ph_panic("unexpected lock mode=%x (%s:%d)", mode, file, line);
  }
}

static void crypto_dynlock_free(struct CRYPTO_dynlock_value *l,
    const char *file, int line)
{
  ph_unused_parameter(file);
  ph_unused_parameter(line);

  pthread_rwlock_destroy(&l->lock);
  ph_mem_free(mt_dynlock, l);
}

static void crypto_lock_cb(int mode, int type, const char *file, int line)
{
  crypto_dynlock_lock(mode, crypto_locks[type], file, line);
}

void ph_library_init_openssl(void)
{
  int i;

  mt_dynlock = ph_memtype_register(&dynlock_def);

  ERR_load_crypto_strings();
  ERR_load_SSL_strings();
  OpenSSL_add_all_algorithms();
  SSL_library_init();

  crypto_locks = calloc(CRYPTO_num_locks(), sizeof(*crypto_locks));
  for (i = 0; i < CRYPTO_num_locks(); i++) {
    crypto_locks[i] = crypto_dynlock_new(__FILE__, __LINE__);
  }
  CRYPTO_set_locking_callback(crypto_lock_cb);

  CRYPTO_set_dynlock_create_callback(crypto_dynlock_new);
  CRYPTO_set_dynlock_lock_callback(crypto_dynlock_lock);
  CRYPTO_set_dynlock_destroy_callback(crypto_dynlock_free);
}


/* vim:ts=2:sw=2:et:
 */


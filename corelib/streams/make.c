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
#include "phenom/memory.h"
#include "phenom/stream.h"
#include "phenom/log.h"
#include <pthread.h>

static ph_memtype_t mt_stm;
static ph_memtype_def_t stm_def = {
  "stream", "stream", 0, 0
};

static pthread_mutexattr_t mtx_attr;

void ph_stm_lock(ph_stream_t *stm)
{
  int res = pthread_mutex_lock(&stm->lock);
  if (ph_unlikely(res != 0)) {
    ph_panic("ph_stm_lock: `Pe%d", res);
  }
}

void ph_stm_unlock(ph_stream_t *stm)
{
  int res = pthread_mutex_unlock(&stm->lock);
  if (ph_unlikely(res != 0)) {
    ph_panic("ph_stm_unlock: `Pe%d", res);
  }
}

ph_stream_t *ph_stm_make(const struct ph_stream_funcs *funcs,
    void *cookie, int flags, uint32_t bufsize)
{
  ph_stream_t *stm;
  int err;

  // Locate the buffer immediate after the stream object
  stm = ph_mem_alloc_size(mt_stm, sizeof(*stm) + PH_STM_UNGET + bufsize);
  if (!stm) {
    return NULL;
  }

  // We only need to zero out the control structure
  memset(stm, 0, sizeof(*stm));

  err = pthread_mutex_init(&stm->lock, &mtx_attr);
  if (err) {
    ph_mem_free(mt_stm, stm);
    errno = err;
    return NULL;
  }

  stm->buf = (unsigned char*)stm + sizeof(*stm) + PH_STM_UNGET;
  stm->bufsize = bufsize;

  stm->funcs = funcs;
  stm->cookie = cookie;
  stm->flags = flags;

  return stm;
}

void ph_stm_destroy(ph_stream_t *stm)
{
  if (stm->flags & PH_STM_FLAG_ONSTACK) {
    ph_panic("ph_stm_destroy: this stream is not a heap instance!");
  }
  ph_mem_free(mt_stm, stm);
}

bool ph_stm_close(ph_stream_t *stm)
{
  if (!ph_stm_flush(stm)) {
    return false;
  }
  if (!stm->funcs->close(stm)) {
    errno = ph_stm_errno(stm);
    return false;
  }
  if ((stm->flags & PH_STM_FLAG_ONSTACK) == 0) {
    ph_mem_free(mt_stm, stm);
  }
  return true;
}

static void stm_init(void)
{
  int err;

  mt_stm = ph_memtype_register(&stm_def);
  if (mt_stm == PH_MEMTYPE_INVALID) {
    ph_panic("ph_stm_init: unable to register memory types");
  }

  err = pthread_mutexattr_init(&mtx_attr);
  if (err) {
    ph_panic("ph_stm_init: mutexattr_init: `Pe%d", err);
  }
  err = pthread_mutexattr_settype(&mtx_attr, PTHREAD_MUTEX_ERRORCHECK);
  if (err) {
    ph_panic("ph_stm_init: mutexattr ERRORCHECK: `Pe%d", err);
  }
}
PH_LIBRARY_INIT_PRI(stm_init, 0, 7)

/* vim:ts=2:sw=2:et:
 */


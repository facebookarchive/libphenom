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
#include "phenom/sysutil.h"

bool ph_stm_copy(ph_stream_t *src, ph_stream_t *dest,
    uint64_t num_bytes, uint64_t *nreadp, uint64_t *nwrotep)
{
  uint64_t nread = 0, nwrote = 0;
  uint64_t remain = num_bytes;
  char lbuf[PH_STM_BUFSIZE];
  int err = 0;

  while (remain > 0 && err == 0) {
    uint64_t r, want = MIN(remain, sizeof(lbuf));

    if (!ph_stm_read(src, lbuf, want, &r)) {
      if (ph_stm_errno(src) == EINTR) {
        continue;
      }
      err = ph_stm_errno(src);
      break;
    }

    if (r == 0) {
      break;
    }

    nread += r;
    remain -= r;

    char *buf = lbuf;
    while (r > 0) {
      uint64_t w;
      if (!ph_stm_write(dest, buf, r, &w)) {
        if (ph_stm_errno(dest) == EINTR) {
          continue;
        }
        err = ph_stm_errno(dest);
        break;
      }

      nwrote += w;
      buf += w;
      r -= w;
    }
  }

  if (nreadp) {
    *nreadp = nread;
  }
  if (nwrotep) {
    *nwrotep = nwrote;
  }

  return err == 0;
}

/* vim:ts=2:sw=2:et:
 */


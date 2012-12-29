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

#include "phenom/defs.h"
#include "phenom/log.h"
#include "phenom/work.h"

static uint8_t log_level = PH_LOG_ERR;

uint8_t phenom_log_level_set(uint8_t level)
{
  uint8_t old = log_level;

  log_level = level;

  return old;
}

uint8_t phenom_log_level_get(void)
{
  return log_level;
}

void phenom_logv(uint8_t level, const char *fmt, va_list ap)
{
  char buf[1024];
  int len;
  int plen;

  if (level > log_level) {
    return;
  }

  plen = snprintf(buf, sizeof(buf), "%" PRIi64 " %u:",
    phenom_time_now(), level);

  len = phenom_vsnprintf(buf + plen, (sizeof(buf) - 1) - plen, fmt, ap);
  len += plen;

  /* ensure it ends with a newline */
  if (buf[len - 1] != '\n') {
    buf[len++] = '\n';
  }

  len = write(STDERR_FILENO, buf, len);
}

void phenom_log(uint8_t level, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  phenom_logv(level, fmt, ap);
  va_end(ap);
}

void phenom_panic(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  phenom_logv(PH_LOG_PANIC, fmt, ap);
  va_end(ap);

  abort();
}

/* vim:ts=2:sw=2:et:
 */


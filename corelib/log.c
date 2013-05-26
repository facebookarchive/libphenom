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

uint8_t ph_log_level_set(uint8_t level)
{
  uint8_t old = log_level;

  log_level = level;

  return old;
}

uint8_t ph_log_level_get(void)
{
  return log_level;
}

void ph_logv(uint8_t level, const char *fmt, va_list ap)
{
  if (level > log_level) {
    return;
  }

  ph_fdprintf(STDERR_FILENO,
      "%" PRIi64 " %u: `Pv%s%p\n",
      ph_time_now(), level, fmt, ph_vaptr(ap));
}

void ph_log(uint8_t level, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  ph_logv(level, fmt, ap);
  va_end(ap);
}

#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
# include <execinfo.h>
#endif

 void ph_log_stacktrace(uint8_t level)
{
#if defined(HAVE_BACKTRACE) && defined(HAVE_BACKTRACE_SYMBOLS)
  void *array[24];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, sizeof(array)/sizeof(array[0]));
  strings = backtrace_symbols(array, size);

  for (i = 0; i < size; i++) {
    ph_log(level, "%s", strings[i]);
  }

  free(strings);
#endif
}

void ph_panic(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  ph_logv(PH_LOG_PANIC, fmt, ap);
  va_end(ap);

  ph_log(PH_LOG_PANIC, "Fatal error detected at:");
  ph_log_stacktrace(PH_LOG_PANIC);
  abort();
}

/* vim:ts=2:sw=2:et:
 */


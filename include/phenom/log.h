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

#ifndef PHENOM_LOG_H
#define PHENOM_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#define PH_LOG_PANIC    0
#define PH_LOG_ALERT    1
#define PH_LOG_CRIT     2
#define PH_LOG_ERR      3
#define PH_LOG_WARN     4
#define PH_LOG_NOTICE   5
#define PH_LOG_INFO     6
#define PH_LOG_DEBUG    7

uint8_t phenom_log_level_set(uint8_t level);
uint8_t phenom_log_level_get(void);

void phenom_log(uint8_t level, const char *fmt, ...);
void phenom_logv(uint8_t level, const char *fmt, va_list ap);

/** Log a PH_LOG_PANIC level message, then abort() */
void phenom_panic(const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((noreturn))
#endif
  ;


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


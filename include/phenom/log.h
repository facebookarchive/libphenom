/*
 * Copyright 2012-2013 Facebook, Inc.
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

/**
 * # Logging
 * Phenom provides simple but useful logging utilities.
 *
 * Each logged message is associated with one of the following
 * severity levels:
 *
 * * `PH_LOG_PANIC` - the world is going to end
 * * `PH_LOG_ALERT` - take notice this is very import
 * * `PH_LOG_CRIT`  - almost as important as alert
 * * `PH_LOG_ERR`   - something bad happened; you should probably look at it
 * * `PH_LOG_WARN`  - something happened but it may not be actionable
 * * `PH_LOG_NOTICE` - somewhat noisy notification about something
 * * `PH_LOG_INFO` - rather more noisy notification of something
 * * `PH_LOG_DEBUG` - noisy diagnostic mode
 *
 * The system has an overall log level that specifies the threshold for
 * which log messages will be allowed to hit the underyling logs.
 *
 * The default is `PH_LOG_ERR`, meaning that a log event must be `PH_LOG_ERR`
 * or higher for the message to hit the logs.
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

/** set the logging level */
uint8_t ph_log_level_set(uint8_t level);

/** get the logging level */
uint8_t ph_log_level_get(void);

/** log something
 *
 * * `level` - the severity level of the event
 * * `fmt` - a ph_printf compatible format string
 *
 * Expands the format string and decorates it with the
 * current timestamp, executing thread name and id,
 * normalizes the line (a missing newline will be added)
 * and sends the result to the log.
 */
void ph_log(uint8_t level, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** log something (va_list)
 *
 * Exactly like `ph_log` but accepts a `va_list` for simpler
 * use in composing functions that also log things.
 *
 * * `level` - the severity level
 * * `fmt` - the ph_printf compatible format string
 * * `ap` - a va_list representing the arguments for the format string
 */
void ph_logv(uint8_t level, const char *fmt, va_list ap);

/** Log a PH_LOG_PANIC level message, then abort()
 *
 * This logs a PANIC level message, logs the current stacktrace
 * and then calls `abort()`.
 *
 * It is intended to be used in situations where the world must
 * have an immediate end.
 * */
void ph_panic(const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 1, 2)))
  __attribute__((noreturn))
#endif
  ;

/** Logs the current call stack at the specified log level.
 * This may block or trigger IO while symbols are loaded.
 * It is intended to be used in last-resort or debug situations,
 * and not in the hot-path.
 * It may be a NOP on some systems.
 */
void ph_log_stacktrace(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


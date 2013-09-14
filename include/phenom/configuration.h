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

#ifndef PHENOM_CONFIGURATION_H
#define PHENOM_CONFIGURATION_H

/**
 * # Configuration
 *
 * libPhenom provides a relatively simple configuration facility; the
 * process has a global configuration expressed as a variant object.
 *
 * There is limited support for mutating the configuration: any
 * values or objects you obtain via the configuration API **MUST**
 * be treated as read-only as they may be accessed from any thread
 * concurrently.
 *
 * If you need to mutate the configuration at runtime (not recommended),
 * then you need to replace the entire configuration object with the
 * new generation of the configuration, and then dispose of the old
 * one.
 */

#include "phenom/variant.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load the configuration file
 *
 * `config_path` may be NULL, in which case a default path is
 * determined by checking for the following:
 *
 * * `getenv("PHENOM_CONFIG_FILE")`
 *
 * The first of these that yields a file that exists will be used
 * as the value for `config_path`.
 *
 * If the filename ends with `.json`, then the file will be
 * interpreted as containing JSON text.
 *
 * A future version may support an alternative format, such as TOML
 * https://github.com/mojombo/toml.
 *
 * If no filename suffix is recognized, the file will be loaded as JSON.
 *
 * Returns `true` if a configuration file was loaded, `false` otherwise.
 */
bool ph_config_load_config_file(const char *config_path);

/** Replace the global configuration
 *
 * Set the configuration to a new generation using the specified
 * variant value.
 *
 * The variant will be considered "owned" by the config system
 * and must be treated as read-only from this point forwards.
 *
 * The old generation, if any, will be released.
 */
void ph_config_set_global(ph_variant_t *cfg);

/** Get the global configuration
 *
 * Returns a reference to the global configuration.
 * You must ph_var_delref() it when you no longer need it.
 *
 * If there is no global configuration, returns a variant
 * representing the null value.
 */
ph_variant_t *ph_config_get_global(void);

/** Perform a JSONPath query on the global configuration
 *
 * Returns a reference on the value it found, if any, else
 * returns a NULL pointer.
 *
 * You must release the returned value using ph_var_delref() when
 * you no longer need the value.
 *
 * See the Variant documentation for details on the JSONPath
 * support provided by phenom.
 */
ph_variant_t *ph_config_query(const char *query);

/** Perform a JSONPath query and return an integer value
 *
 * A convenience wrapper around performing a global query
 * and interpreting the result as an integer with a default
 * value.
 */
int64_t ph_config_query_int(const char *query, int64_t defval);

/** Perform a JSONPath query and return an integer value
 *
 * A convenience wrapper around performing a global query
 * and interpreting the result as an integer with a default
 * value.
 *
 * This function expands query using ph_vprintf_core() so that
 * you can parameterize your lookup.
 */
int64_t ph_config_queryf_int(int64_t defval, const char *query, ...);

/** Perform a JSONPath query and return a double value
 *
 * A convenience wrapper around performing a global query
 * and interpreting the result as a double with a default
 * value.
 */
double ph_config_query_double(const char *query, double defval);

/** Perform a JSONPath query and return a string value
 *
 * A convenience wrapper around performing a global query
 * and interpreting the result as a string with a default
 * value.
 *
 * You must call ph_string_delref() on the returned string when
 * you no longer need it.
 */
ph_string_t *ph_config_query_string(const char *query, ph_string_t *defval);

/** Perform a JSONPath query and return a string value
 *
 * A convenience wrapper around performing a global query
 * and interpreting the result as a string with a default
 * value.
 *
 * You must call ph_string_delref() on the returned string when
 * you no longer need it.
 *
 * Note that this will create a string wrapper for defval if it does not
 * exist.  If you are querying for this value frequently, you should
 * consider using ph_config_query_string instead to reduce your heap overhead.
 */
ph_string_t *ph_config_query_string_cstr(const char *query, const char *defval);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


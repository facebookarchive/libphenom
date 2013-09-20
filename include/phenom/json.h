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

#ifndef PHENOM_JSON_H
#define PHENOM_JSON_H

#include "phenom/defs.h"
#include "phenom/stream.h"
#include "phenom/variant.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * # JSON Support
 *
 * libPhenom provides JSON encoding and decoding support functions.
 * These are implemented in terms of the variant datatype; you
 * may encode from a variant to JSON and vice-versa.
 *
 * ## Loading and decoding JSON
 *
 * This is accomplished using ph_json_load_stream(), ph_json_load_string()
 * or ph_json_load_cstr().
 *
 * You may specify one or more of the following flags to alter the load
 * behavior:
 *
 * * `PH_JSON_REJECT_DUPLICATES` - fail to decode if objects contain
 *   more than one copy of the same key.
 * * `PH_JSON_DECODE_ANY` - the loader will normally only successfully
 *   complete if the JSON represents either an array or an object.
 *   Setting this flag allows any JSON value to be loaded.
 * * `PH_JSON_DISABLE_EOF_CHECK` - the loader will normally look ahead
 *   and expect to see EOF after it has successfully decoded a JSON value.
 *   If EOF is not found, the decode will fail on the basis that there is
 *   trailing garbage so the input is probably invalid.  Specifying this
 *   flag will disable the EOF check.
 *   If used together with `PH_JSON_DECODE_ANY`, the decoder may read
 *   one extra code point (up to 4 bytes of input); for example, if the
 *   input is `4true` the decoder correctly decodes `4` but also reads the
 *   `t` character.  It is recommended that you separate such values by
 *   whitespace if you are reading multiple consecutive non array, non object
 *   values.
 *
 * ### Handling load errors
 *
 * The load functions allow you to specify an optional *error* parameter.
 * If present, it will be updated to hold some error context that you can
 * present to the user.
 *
 * It has the following fields:
 *
 * * `line` - the line number of the problematic sequence
 * * `column` - the offset within the line measured in code points
 * * `position` - the byte offset within the JSON stream
 * * `text` - a human readable error message explaining the problem
 * * `transient` - if true, indicates that retrying later might yield success
 *
 * If the load operation was successful, the `position` field is updated to
 * hold the number of bytes consumed from the input.  This is useful in
 * conjunction with `PH_JSON_DISABLE_EOF_CHECK` to read multiple consecutive
 * JSON values.
 *
 * ## Dumping and encoding JSON
 *
 * This is accomplished using ph_json_dump_stream() or ph_json_dump_string().
 *
 * You may specify one or more of the following flags to alter the dump
 * behavior:
 *
 * * `PH_JSON_INDENT(n)` - The default representation is a single line of
 *   JSON text.  Using `PH_JSON_INDENT(2)` will use a two-space indentation
 *   for objects and arrays, pretty-printing the output.
 * * `PH_JSON_COMPACT` - when set, suppresses some spaces and makes the
 *   output more compact and less human readable.
 * * `PH_JSON_ENSURE_ASCII` - when set, causes the output to be restricted
 *   to 7-bit ASCII characters, escaping any characters outside this range.
 * * `PH_JSON_SORT_KEYS` - when set, object values are walked in a sorted
 *   order.  This is more expensive but is useful for textual comparisons.
 * * `PH_JSON_ESCAPE_SLASH` - escapes the `/` character in strings, which
 *   is useful in cases where the JSON will be embedded in e.g. HTML output.
 */


#define PH_JSON_REJECT_DUPLICATES 0x1
#define PH_JSON_DISABLE_EOF_CHECK 0x2
#define PH_JSON_DECODE_ANY        0x4

#define PH_JSON_INDENT(n)      (n & 0x1F)
#define PH_JSON_COMPACT        0x20
#define PH_JSON_ENSURE_ASCII   0x40
#define PH_JSON_SORT_KEYS      0x80
#define PH_JSON_ESCAPE_SLASH   0x100

/** Parse JSON from a stream
 *
 * Attempt to parse and decode a JSON encoded data stream.
 * Returns a variant instance if successful.
 * On failure, updates the provided `err` pointer to contain
 * some context on the failure.
 */
ph_variant_t *ph_json_load_stream(ph_stream_t *stm, uint32_t flags,
    ph_var_err_t *err);

/** Parse JSON from a string
 *
 * Attempt to parse and decode a JSON encoded string.
 * Returns a variant instance if successful.
 * On failure, updates the provided `err` pointer to contain
 * some context on the failure.
 */
ph_variant_t *ph_json_load_string(ph_string_t *str, uint32_t flags,
    ph_var_err_t *err);

/** Parse JSON from a C-string
 */
ph_variant_t *ph_json_load_cstr(const char *cstr, uint32_t flags,
    ph_var_err_t *err);

/** Encode a variant as JSON, write to stream
 *
 * Given a variant, encodes it as JSON and writes to the provided
 * stream.
 */
ph_result_t ph_json_dump_stream(ph_variant_t *obj,
    ph_stream_t *stm, uint32_t flags);

/** Encode a variant as JSON, append to string
 *
 * Given a variant, encodes it as JSON and appends to the provided
 * string.
 */
ph_result_t ph_json_dump_string(ph_variant_t *var,
    ph_string_t *str, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


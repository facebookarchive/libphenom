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
#include "phenom/string.h"
#include "phenom/stream.h"
#include "tap.h"

static ph_memtype_def_t mt_def = { "test", "misc", 0, 0 };
static ph_memtype_t mt_misc = 0;

int main(int argc, char **argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(22);

  mt_misc = ph_memtype_register(&mt_def);

  {
    ph_sockaddr_t sa;
    PH_STRING_DECLARE_GROW(str, 128, mt_misc);

    is(ph_sockaddr_set_v4(&sa, NULL, 42), PH_OK);
    is(ph_sockaddr_print(&sa, &str, true), PH_OK);
    ok(ph_string_equal_cstr(&str, "[0.0.0.0]:42"), "printed with port");
    ph_string_reset(&str);
    is(ph_sockaddr_print(&sa, &str, false), PH_OK);
    ok(ph_string_equal_cstr(&str, "0.0.0.0"), "printed no port");

    ph_string_reset(&str);
    is(ph_sockaddr_set_v4(&sa, "10.11.12.13", 1337), PH_OK);
    is(ph_sockaddr_print(&sa, &str, true), PH_OK);
    ok(ph_string_equal_cstr(&str, "[10.11.12.13]:1337"), "printed with port");
    ph_string_reset(&str);
    is(ph_sockaddr_print(&sa, &str, false), PH_OK);
    ok(ph_string_equal_cstr(&str, "10.11.12.13"), "printed no port");

    ph_string_reset(&str);
    is(ph_sockaddr_set_v6(&sa, NULL, 42), PH_OK);
    is(ph_sockaddr_print(&sa, &str, true), PH_OK);
    ok(ph_string_equal_cstr(&str, "[::]:42"), "printed with port");
    ph_string_reset(&str);
    is(ph_sockaddr_print(&sa, &str, false), PH_OK);
    ok(ph_string_equal_cstr(&str, "::"), "printed no port");

    ph_string_reset(&str);
    is(ph_sockaddr_set_v6(&sa, "::1", 1337), PH_OK);
    is(ph_sockaddr_print(&sa, &str, true), PH_OK);
    ok(ph_string_equal_cstr(&str, "[::1]:1337"), "printed with port");
    ph_string_reset(&str);
    is(ph_sockaddr_print(&sa, &str, false), PH_OK);
    ok(ph_string_equal_cstr(&str, "::1"), "printed no port");

    ph_string_reset(&str);
    ph_string_printf(&str, "`P{sockaddr:%p}", (void*)&sa);
    ok(ph_string_equal_cstr(&str, "[::1]:1337"), "printfed with port");

    ph_string_reset(&str);
    ph_string_printf(&str, "`P{sockaddr:%p}", NULL);
    ok(ph_string_equal_cstr(&str, "sockaddr:null"), "null safe");
  }

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */


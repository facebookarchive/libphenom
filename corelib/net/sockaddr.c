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

#include "phenom/socket.h"
#include "phenom/log.h"
#include "phenom/printf.h"
#include <netdb.h>

PH_TYPE_FORMATTER_FUNC(sockaddr)
{
  ph_sockaddr_t *sa = object;
  PH_STRING_DECLARE_STACK(str, 132);

  if (!sa) {
#define null_sockaddr_string "sockaddr:null"
    funcs->print(print_arg, null_sockaddr_string,
        sizeof(null_sockaddr_string) - 1);
    return sizeof(null_sockaddr_string)-1;
  }

  if (ph_sockaddr_print(sa, &str, true) == PH_OK) {
    return funcs->print(print_arg, str.buf, str.len) ? str.len : 0;
  }

  ph_string_printf(&str, "<bad sockaddr:%p>", object);
  return funcs->print(print_arg, str.buf, str.len) ? str.len : 0;
}

ph_result_t ph_sockaddr_set_v4(ph_sockaddr_t *sa,
    const char *addr, uint16_t port)
{
  int res = 1;

  memset(sa, 0, sizeof(*sa));
  if (addr) {
    res = inet_pton(AF_INET, addr, &sa->sa.v4.sin_addr);
  }
  /* the memset above zeroes out the struct, which is the same
   * bit pattern as the any address
  else {
    sa->sa.v4.sin_addr.s_addr = INADDR_ANY;
  }
  */

  if (res > 0) {
    sa->family = AF_INET;
    sa->sa.v4.sin_family = AF_INET;
    sa->sa.v4.sin_port = htons(port);
    return PH_OK;
  }

  return PH_ERR;
}

void ph_sockaddr_set_port(ph_sockaddr_t *sa, uint16_t port)
{
  switch (sa->family) {
    case AF_INET6:
      sa->sa.v6.sin6_port = htons(port);
      break;
    case AF_INET:
      sa->sa.v4.sin_port = htons(port);
      break;
  }
}

ph_result_t ph_sockaddr_set_v6(
    ph_sockaddr_t *sa,
    const char *addr,
    uint16_t port)
{
  int res = 1;
  struct addrinfo *ai, hints;

  memset(sa, 0, sizeof(*sa));
  if (!addr) {
    /* the memset above zeroes the struct, which is the same
     * bit pattern as the any address
     */
    sa->family = AF_INET6;
    sa->sa.v6.sin6_family = AF_INET6;
    sa->sa.v6.sin6_port = htons(port);
    return PH_OK;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_flags = AI_NUMERICHOST|AI_V4MAPPED;
  hints.ai_family = AF_INET6;

  res = getaddrinfo(addr, NULL, &hints, &ai);
  if (res) {
    return PH_ERR;
  }

  sa->family = ai->ai_family;

  switch (sa->family) {
    case AF_INET6:
      memcpy(&sa->sa.sa, ai->ai_addr, sizeof(sa->sa.v6));
      sa->sa.v6.sin6_port = htons(port);
      freeaddrinfo(ai);
      return PH_OK;

    case AF_INET:
      memcpy(&sa->sa.sa, ai->ai_addr, sizeof(sa->sa.v4));
      sa->sa.v4.sin_port = htons(port);
      freeaddrinfo(ai);
      return PH_OK;

    default:
      freeaddrinfo(ai);
      return PH_ERR;
  }
}

ph_result_t ph_sockaddr_set_unix(
    ph_sockaddr_t *addr,
    const char *path,
    unsigned int pathlen)
{
  if (!path) {
    return PH_ERR;
  }
  if (*path == 0) {
    /* explicitly do not support the Linux unix abstract namespace */
    return PH_ERR;
  }

  if (pathlen == 0) {
    pathlen = strlen(path);
  }

  /* Linux defines UNIX_PATH_MAX, but there is no POSIX requirement.
   * Let's just ask the compiler for the size */
  if (pathlen + 1 /* \0 */ >= sizeof(addr->sa.nix.sun_path)) {
    return PH_ERR;
  }

  memset(addr, 0, sizeof(*addr));
  addr->family = AF_UNIX;
  addr->sa.nix.sun_family = AF_UNIX;

  memcpy(&addr->sa.nix.sun_path, path, pathlen);
  return PH_OK;
}

ph_result_t ph_sockaddr_print(ph_sockaddr_t *sa,
    ph_string_t *str, bool want_port)
{
  int ret;
  char buf[128];
  char pbuf[32];

  switch (sa->family) {
    case AF_UNIX:
      return ph_string_append_cstr(str, sa->sa.nix.sun_path);

    case AF_INET:
    case AF_INET6:
      ret = getnameinfo(&sa->sa.sa, ph_sockaddr_socklen(sa),
              buf, sizeof(buf), pbuf, sizeof(pbuf),
              NI_NUMERICHOST|NI_NUMERICSERV);

      if (ret) {
        return PH_ERR;
      }
      if (want_port) {
        ph_string_printf(str, "[%s]:%s", buf, pbuf);
        return PH_OK;
      }
      return ph_string_append_cstr(str, buf);
  }
  return PH_ERR;
}

ph_result_t ph_sockaddr_set_from_addrinfo(
    ph_sockaddr_t *sa,
    struct addrinfo *ai)
{
  memset(sa, 0, sizeof(*sa));
  sa->family = ai->ai_family;

  switch (sa->family) {
    case AF_INET6:
      memcpy(&sa->sa.sa, ai->ai_addr, sizeof(sa->sa.v6));
      return PH_OK;

    case AF_INET:
      memcpy(&sa->sa.sa, ai->ai_addr, sizeof(sa->sa.v4));
      return PH_OK;

    default:
      return PH_ERR;
  }
}

ph_result_t ph_sockaddr_set_from_hostent(
    ph_sockaddr_t *sa,
    struct hostent *ent)
{
  memset(sa, 0, sizeof(*sa));
  sa->family = ent->h_addrtype;

  switch (sa->family) {
    case AF_INET6:
      sa->sa.v6.sin6_family = sa->family;
      memcpy(&sa->sa.v6.sin6_addr, ent->h_addr_list[0],
          sizeof(sa->sa.v6.sin6_addr));
      return PH_OK;

    case AF_INET:
      sa->sa.v4.sin_family = sa->family;
      memcpy(&sa->sa.v4.sin_addr, ent->h_addr_list[0],
          sizeof(sa->sa.v4.sin_addr));
      return PH_OK;

    default:
      return PH_ERR;
  }
}


/* vim:ts=2:sw=2:et:
 */


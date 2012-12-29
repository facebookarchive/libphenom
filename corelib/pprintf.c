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
#include "phenom/sysutil.h"

/* These functions are built on top of the code found in vprintf.c */

struct fixed_buf {
  char *start;
  char *end;
};

static bool fixed_buf_print(void *arg, const char *src, size_t len)
{
  struct fixed_buf *buf = arg;
  uint32_t avail = buf->end - buf->start;

  if (avail < len) {
    len = avail;
  }

  memcpy(buf->start, src, len);
  buf->start += len;

  return true;
}

static bool fixed_buf_flush(void *arg)
{
  struct fixed_buf *buf = arg;

  *buf->start = '\0';
  return true;
}

static struct phenom_vprintf_funcs fixed_buf_funcs = {
  fixed_buf_print,
  fixed_buf_flush
};

int phenom_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
  struct fixed_buf fb = { buf, buf + size - 1 };

  return phenom_vprintf_core(&fb, &fixed_buf_funcs, fmt, ap);
}

int phenom_snprintf(char *buf, size_t size, const char *fmt, ...)
{
  va_list ap;
  int res;

  va_start(ap, fmt);
  res = phenom_vsnprintf(buf, size, fmt, ap);
  va_end(ap);

  return res;
}

struct fd_buffer {
  int fd;
  char *next;
  char start[1024];
};

static bool fd_buf_flush(void *arg)
{
  struct fd_buffer *buf = arg;
  int res;
  char *cursor;

  if (buf->next == buf->start) {
    return true;
  }

  cursor = buf->start;

  while (cursor < buf->next) {
    res = write(buf->fd, cursor, buf->next - cursor);

    if (res <= 0) {
      return false;
    }

    /* short write, try some more */
    cursor += res;
  }

  buf->next = buf->start;
  return true;
}

static bool fd_buf_print(void *arg, const char *src, size_t len)
{
  struct fd_buffer *buf = arg;
  uint32_t avail;

  while (len > 0) {
    avail = sizeof(buf->start) - (buf->next - buf->start);

    if (avail == 0) {
      if (!fd_buf_flush(buf)) {
        return false;
      }
      avail = sizeof(buf->start) - (buf->next - buf->start);
    }

    if (avail > len) {
      avail = len;
    }

    memcpy(buf->next, src, avail);
    buf->next += avail;
    src += avail;
    len -= avail;
  }

  return true;
}

static struct phenom_vprintf_funcs fd_buf_funcs = {
  fd_buf_print,
  fd_buf_flush
};

int phenom_vfdprintf(int fd, const char *fmt, va_list ap)
{
  struct fd_buffer buf;

  buf.fd = fd;
  buf.next = buf.start;

  return phenom_vprintf_core(&buf, &fd_buf_funcs, fmt, ap);
}

int phenom_fdprintf(int fd, const char *fmt, ...)
{
  va_list ap;
  int res;

  va_start(ap, fmt);
  res = phenom_vfdprintf(fd, fmt, ap);
  va_end(ap);

  return res;
}

struct print_n_grow {
  phenom_memtype_t mt;
  uint32_t allocd;
  uint32_t used;
  char *mem;
};

static bool grow_print(void *arg, const char *src, size_t len)
{
  struct print_n_grow *grow = arg;
  uint32_t avail = grow->allocd - grow->used;

  if (avail < len) {
    uint32_t target = phenom_power_2(grow->used + len + 1);
    char *revised;

    if (target < 128) {
      target = 128;
    }

    if (grow->mt == PHENOM_MEMTYPE_INVALID) {
      revised = realloc(grow->mem, target);
    } else {
      revised = phenom_mem_realloc(grow->mt, grow->mem, target);
    }

    if (!revised) {
      return false;
    }

    // under report to make the unsigned avail math easier.
    // This reserves us room for the NUL byte
    grow->allocd = target - 1;
    grow->mem = revised;
  }

  memcpy(grow->mem + grow->used, src, len);
  grow->used += len;

  return true;
}

static bool grow_flush(void *arg)
{
  struct print_n_grow *grow = arg;

  grow->mem[grow->used] = '\0';
  return true;
}

static struct phenom_vprintf_funcs grow_buf_funcs = {
  grow_print,
  grow_flush
};

int phenom_vasprintf(char **strp, const char *fmt, va_list ap)
{
  struct print_n_grow grow = {
    PHENOM_MEMTYPE_INVALID, 0, 0, 0
  };
  int ret;

  ret = phenom_vprintf_core(&grow, &grow_buf_funcs, fmt, ap);

  if (ret == -1) {
    if (grow.mem) {
      free(grow.mem);
      grow.mem = NULL;
    }
  }
  *strp = grow.mem;
  return ret;
}

int phenom_asprintf(char **strp, const char *fmt, ...)
{
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = phenom_vasprintf(strp, fmt, ap);
  va_end(ap);

  return ret;
}


int phenom_vmtsprintf(phenom_memtype_t memtype, char **strp,
    const char *fmt, va_list ap)
{
  struct print_n_grow grow = {
    memtype, 0, 0, 0
  };
  int ret;

  if (memtype == PHENOM_MEMTYPE_INVALID) {
    errno = EINVAL;
    return -1;
  }

  ret = phenom_vprintf_core(&grow, &grow_buf_funcs, fmt, ap);

  if (ret == -1) {
    if (grow.mem) {
      phenom_mem_free(memtype, grow.mem);
      grow.mem = NULL;
    }
  }
  *strp = grow.mem;
  return ret;
}

int phenom_mtsprintf(phenom_memtype_t memtype, char **strp,
    const char *fmt, ...)
{
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = phenom_vmtsprintf(memtype, strp, fmt, ap);
  va_end(ap);

  return ret;
}



/* vim:ts=2:sw=2:et:
 */


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

#include "phenom/buffer.h"
#include "phenom/memory.h"
#include "phenom/refcnt.h"
#include "phenom/queue.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/printf.h"
#include <ctype.h>

struct ph_buf {
  ph_refcnt_t ref;
  ph_buf_t *slice;
  uint8_t *buf;
  uint64_t size;
  ph_memtype_t memtype;
};

struct ph_bufq_ent {
  PH_STAILQ_ENTRY(ph_bufq_ent) ent;
  ph_buf_t *buf;
  // Offset into the buf of the data that is yet to be consumed
  uint64_t rpos;
  // Offset at which to append further data
  uint64_t wpos;
};

struct ph_bufq {
  PH_STAILQ_HEAD(bufqhead, ph_bufq_ent) fifo;
  // Maximum amount of storage to allow
  uint64_t max_size;
  uint64_t max_record_size;

  bool last_overflow;
  uint64_t last_search_index;
  struct ph_bufq_ent *last_search_position;

  char last_delim[16];
  uint32_t last_delim_len;
};

static ph_memtype_def_t defs[] = {
  { "buffer", "object", sizeof(ph_buf_t), PH_MEM_FLAGS_ZERO },
  { "buffer", "8k", 8*1024, 0 },
  { "buffer", "16k", 16*1024, 0 },
  { "buffer", "32k", 32*1024, 0 },
  { "buffer", "64k", 64*1024, 0 },
  { "buffer", "vsize", 0, 0 },
  { "buffer", "queue", sizeof(ph_bufq_t), PH_MEM_FLAGS_ZERO },
  { "buffer", "queue_ent", sizeof(struct ph_bufq_ent), PH_MEM_FLAGS_ZERO },
};

static struct {
  ph_memtype_t obj, f8k, f16k, f32k, f64k, vsize, queue, queue_ent;
} mt;

static void buffer_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.obj);
}

PH_LIBRARY_INIT(buffer_init, 0)

PH_TYPE_FORMATTER_FUNC(buf)
{
  ph_buf_t *buf = object;
  char txt[64];
  int len, sample, i;
  uint8_t *data = ph_buf_mem(buf);

  if (!buf) {
#define null_buffer_string "buf:null"
    funcs->print(print_arg, null_buffer_string, sizeof(null_buffer_string)-1);
    return sizeof(null_buffer_string)-1;
  }

  len = ph_snprintf(txt, sizeof(txt),
      "buf{%p: len=%" PRIu64 "} = \"", object, ph_buf_len(buf));

  if (!funcs->print(print_arg, txt, len)) {
    return 0;
  }

  sample = MIN(ph_buf_len(buf), 64);
  for (i = 0; i < sample; i++) {
    if (isprint(data[i])) {
      funcs->print(print_arg, (char*) data + i, 1);
      len++;
      continue;
    }

    switch (data[i]) {
      case '\r':
        funcs->print(print_arg, "\\r", 2);
        len += 2;
        break;
      case '\n':
        funcs->print(print_arg, "\\n", 2);
        len += 2;
        break;
      case '\t':
        funcs->print(print_arg, "\\t", 2);
        len += 2;
        break;
      default:
        ph_snprintf(txt, sizeof(txt), "\\x%02x", data[i]);
        funcs->print(print_arg, txt, 4);
        len += 4;
    }
  }
  funcs->print(print_arg, "\"", 1);

  return len + 1;
}

static inline uint64_t select_size(uint64_t size, ph_memtype_t *mtp)
{
  ph_memtype_t t;

  if (size < 8192) {
    t = mt.f8k;
    size = 8192;
  } else if (size < 16 * 1024) {
    t = mt.f16k;
    size = 16 * 1024;
  } else if (size < 32 * 1024) {
    t = mt.f32k;
    size = 32 * 1024;
  } else if (size < 64 * 1024) {
    t = mt.f64k;
    size = 64 * 1024;
  } else {
    t = mt.vsize;
    size = 0;
  }

  if (mtp) {
    *mtp = t;
  }
  return size;
}

ph_buf_t *ph_buf_new(uint64_t size)
{
  ph_buf_t *buf;
  uint64_t selected;

  buf = ph_mem_alloc(mt.obj);

  if (!buf) {
    return NULL;
  }

  /* Find the right size bucket.
   * We report their requested size, even if the bucket we pick is
   * bigger, so that we can sanely handle the case when they copy
   * memory of a known size into a buffer.
   * If they requested a 0 size, we report the bucket size as this
   * pattern is asking for some buffer space and they don't care
   * how big it is, so long as they know how big it is. */
  buf->size = size;
  selected = select_size(size, &buf->memtype);
  if (size == 0) {
    buf->size = selected;
  }

  if (buf->memtype == mt.vsize) {
    buf->buf = ph_mem_alloc_size(buf->memtype, size);
    if (!buf->buf) {
      ph_mem_free(mt.obj, buf);
      return NULL;
    }
  }

  if (!buf->buf) {
    buf->buf = ph_mem_alloc(buf->memtype);
    if (!buf->buf) {
      ph_mem_free(mt.obj, buf);
      return NULL;
    }
  }

  buf->ref = 1;
  return buf;
}

void ph_buf_addref(ph_buf_t *buf)
{
  ph_refcnt_add(&buf->ref);
}

void ph_buf_delref(ph_buf_t *buf)
{
  if (!ph_refcnt_del(&buf->ref)) {
    return;
  }

  if (buf->slice) {
    ph_buf_delref(buf->slice);
  } else {
    ph_mem_free(buf->memtype, buf->buf);
  }
  ph_mem_free(mt.obj, buf);
}

ph_buf_t *ph_buf_slice(ph_buf_t *buf, uint64_t start, uint64_t len)
{
  ph_buf_t *slice;

  if (len == 0) {
    if (start >= buf->size) {
      return NULL;
    }
    len = buf->size - start;
  }

  if (start + len > buf->size) {
    return NULL;
  }

  if (start == 0 && len == buf->size) {
    // Just return another ref on this one
    ph_buf_addref(buf);
    return buf;
  }

  slice = ph_mem_alloc(mt.obj);
  if (!slice) {
    return NULL;
  }

  slice->slice = buf;
  ph_buf_addref(buf);
  slice->buf = buf->buf + start;
  slice->size = len;
  slice->ref = 1;

  return slice;
}

bool ph_buf_copy_mem(ph_buf_t *dest, const void *mem, uint64_t len,
    uint64_t dest_start)
{
  if (dest_start + len > dest->size) {
    return false;
  }

  memmove(dest->buf + dest_start, mem, len);
  return true;
}

bool ph_buf_copy(ph_buf_t *src, ph_buf_t *dest, uint64_t start, uint64_t len,
    uint64_t dest_start)
{
  if (start + len > src->size) {
    return false;
  }
  if (dest_start + len > dest->size) {
    return false;
  }
  memmove(dest->buf + dest_start, src->buf + start, len);
  return true;
}

bool ph_buf_set(ph_buf_t *buf, int value, uint64_t start, uint64_t len)
{
  if (start + len > buf->size) {
    return false;
  }
  memset(buf->buf + start, value, len);
  return true;
}

uint64_t ph_buf_len(ph_buf_t *buf)
{
  return buf->size;
}

uint8_t *ph_buf_mem(ph_buf_t *buf)
{
  return buf->buf;
}

ph_buf_t *ph_buf_concat(uint64_t total_length, uint32_t num_buffers,
    ph_buf_t **buffers, uint64_t first_offset)
{
  uint32_t i;
  uint64_t off;
  ph_buf_t *buf;

  if (total_length == 0) {
    for (i = 0; i < num_buffers; i++) {
      total_length += buffers[i]->size;
    }
    total_length -= first_offset;
  }

  buf = ph_buf_new(total_length);
  if (!buf) {
    return NULL;
  }

  for (off = 0, i = 0; off < total_length && i < num_buffers; i++) {
    uint64_t len = buffers[i]->size;
    if (off + len > total_length) {
      len = total_length - off;
    }
    if (!ph_buf_copy(buffers[i], buf,
        i == 0 ? first_offset : 0,
        len, off)) {
      ph_buf_delref(buf);
      return NULL;
    }
    off += buffers[i]->size;
  }

  return buf;
}

static struct ph_bufq_ent *q_add_new_buf(ph_bufq_t *q, uint64_t bufsize)
{
  struct ph_bufq_ent *ent;

  ent = ph_mem_alloc(mt.queue_ent);
  if (!ent) {
    return NULL;
  }

  ent->buf = ph_buf_new(bufsize);
  if (!ent->buf) {
    ph_mem_free(mt.queue_ent, ent);
    return NULL;
  }

  PH_STAILQ_INSERT_TAIL(&q->fifo, ent, ent);

  return ent;
}

ph_bufq_t *ph_bufq_new(uint64_t max_size)
{
  ph_bufq_t *q;

  q = ph_mem_alloc(mt.queue);
  if (!q) {
    return NULL;
  }

  PH_STAILQ_INIT(&q->fifo);
  q->max_size = max_size;

  if (!q_add_new_buf(q, 0)) {
    ph_mem_free(mt.queue, q);
    return NULL;
  }

  return q;
}

void ph_bufq_set_max_record_size(ph_bufq_t *q, uint64_t size) {
  q->max_record_size = size;
}

uint64_t ph_bufq_get_max_record_size(ph_bufq_t *q) {
  return q->max_record_size;
}

ph_result_t ph_bufq_append(ph_bufq_t *q, const void *buf, uint64_t len,
    uint64_t *added_bytes)
{
  struct ph_bufq_ent *last, *append;
  const char *cbuf = buf;
  uint64_t avail = 0;
  uint64_t consumed = 0;

  last = PH_STAILQ_LAST(&q->fifo, ph_bufq_ent, ent);
  if (last) {
    avail = ph_buf_len(last->buf) - last->wpos;
    avail = MIN(avail, len);
  }

  // Top-off
  if (avail) {
    ph_buf_copy_mem(last->buf, cbuf, avail, last->wpos);
    last->wpos += avail;
    len -= avail;
    cbuf += avail;
    consumed += avail;
  }

  if (len == 0) {
    if (added_bytes) {
      *added_bytes = consumed;
    }
    return PH_OK;
  }

  // We need more storage
  append = q_add_new_buf(q, len);

  if (!append) {
    if (added_bytes) {
      *added_bytes = consumed;
    }
    if (consumed) {
      return PH_OK;
    }
    return PH_NOMEM;
  }

  ph_buf_copy_mem(append->buf, cbuf, len, 0);
  append->wpos = len;
  consumed += len;

  if (added_bytes) {
    *added_bytes = consumed;
  }

  return PH_OK;
}

static uint64_t bufq_len(ph_bufq_t *q, uint32_t *num_ents)
{
  uint64_t avail = 0;
  struct ph_bufq_ent *ent;
  uint32_t n = 0;

  PH_STAILQ_FOREACH(ent, &q->fifo, ent) {
    avail += ent->wpos - ent->rpos;
    n++;
  }

  if (num_ents) {
    *num_ents = n;
  }

  return avail;
}

uint64_t ph_bufq_len(ph_bufq_t *q)
{
  return bufq_len(q, NULL);
}

static void gc_bufq(ph_bufq_t *q)
{
  struct ph_bufq_ent *ent, *last;

  last = PH_STAILQ_LAST(&q->fifo, ph_bufq_ent, ent);
  while (true) {
    ent = PH_STAILQ_FIRST(&q->fifo);
    if (!ent) {
      // Nothing to prune
      break;
    }

    if (ent->rpos != ent->wpos) {
      // Data to be consumed: can't prune it
      break;
    }

    if (ent == last && ent->wpos < ph_buf_len(ent->buf)) {
      // It has room for some appends
      break;
    }

    // We don't need this one
    PH_STAILQ_REMOVE_HEAD(&q->fifo, ent);
    ph_buf_delref(ent->buf);
    ph_mem_free(mt.queue_ent, ent);
  }
}

static ph_buf_t *slice_bufq(ph_bufq_t *q, uint64_t len, bool consume)
{
  struct ph_bufq_ent *ent;
  uint64_t copy_len, next = 0;
  uint32_t n;
  ph_buf_t *buf = NULL;

  if (len == 0 || len > bufq_len(q, &n)) {
    return NULL;
  }

  if (n == 1) {
    // We can simply slice the first one
    ent = PH_STAILQ_FIRST(&q->fifo);
    buf = ph_buf_slice(ent->buf, ent->rpos, len);

    if (!buf) {
      return NULL;
    }

    if (consume) {
      ent->rpos += len;
      gc_bufq(q);
    }

    return buf;
  }

  buf = ph_buf_new(len);
  if (!buf) {
    return NULL;
  }

  PH_STAILQ_FOREACH(ent, &q->fifo, ent) {
    copy_len = MIN(ent->wpos - ent->rpos, len - next);
    ph_buf_copy(ent->buf, buf, ent->rpos, copy_len, next);
    next += copy_len;
    if (consume) {
      ent->rpos += copy_len;
    }
    if (next == len) {
        break;
    }
  }

  if (consume) {
    gc_bufq(q);
  }

  return buf;
}

ph_buf_t *ph_bufq_consume_bytes(ph_bufq_t *q, uint64_t len)
{
  return slice_bufq(q, len, true);
}

ph_buf_t *ph_bufq_peek_bytes(ph_bufq_t *q, uint64_t len)
{
  return slice_bufq(q, len, false);
}

// If the end of ent->buf is occupied by a prefix string of delim, return
// the number of suffix bytes that we need to search into the next ent
static uint32_t partial_match(struct ph_bufq_ent *ent, const char *delim,
    uint32_t delim_len)
{
  uint32_t slen;
  uint8_t *bend = ph_buf_mem(ent->buf) + ent->wpos;
  uint8_t *bstart = ph_buf_mem(ent->buf) + ent->rpos;

  for (slen = delim_len - 1; slen > 0; slen--) {
    uint8_t *buf = bend - slen;
    if (buf < bstart) {
      continue;
    }

    if (memcmp(buf, delim, slen) == 0) {
      return delim_len - slen;
    }
  }

  return 0;
}

static inline void clear_searchstate(ph_bufq_t *q) {
  q->last_overflow = false;
  q->last_search_index = 0;
  q->last_search_position = NULL;
  q->last_delim_len = 0;
}

static inline void set_searchstate(ph_bufq_t *q, uint64_t searched,
    struct ph_bufq_ent *ent, const char *delim, uint32_t delim_len) {
  if (delim_len > sizeof(q->last_delim)) {
    clear_searchstate(q);
    return;
  }

  q->last_search_index = searched;
  q->last_search_position = ent;
  q->last_delim_len = delim_len;

  memcpy(q->last_delim, delim, delim_len);
}

// Locate the record.
// We look for the delimiter text; if we find it in the buffer queue, we return
// the length from the start of the queue to the end of the delimiter.
// Our caller can then pass this length to consume_bytes or peek_bytes to obtain
// the appropriate slice.
// Returns 0 if the delimiter was not found, sets *overflow to true if we
// overflow max_size while reading.
static uint64_t find_record(ph_bufq_t *q, const char *delim, uint32_t delim_len,
    bool *overflow)
{
  struct ph_bufq_ent *ent, *next;
  uint64_t searched = 0;
  uint32_t slen;

  *overflow = false;

  if (delim_len == q->last_delim_len &&
      delim_len <= sizeof(q->last_delim) &&
      memcmp(delim, q->last_delim, delim_len) == 0) {
    ent = q->last_search_position;
    searched = q->last_search_index;
  } else {
    ent = PH_STAILQ_FIRST(&q->fifo);

    clear_searchstate(q);
  }

  for (; ent; ent = PH_STAILQ_NEXT(ent, ent)) {
    uint8_t *bstart = ph_buf_mem(ent->buf) + ent->rpos;
    uint64_t len = ent->wpos - ent->rpos;
    uint8_t *found;

    found = memmem(bstart, len, delim, delim_len);

    if (found) {
      searched += (found + delim_len) - bstart;

      clear_searchstate(q);

      return searched;
    }

    next = PH_STAILQ_NEXT(ent, ent);
    if (next == NULL) {
      // Not found, save position for next search.
      // Note we have to make sure we re-search this buffer next time,
      // as we may have a straddle when more data is there.
      set_searchstate(q, searched, ent, delim, delim_len);

      return 0;
    }

    // check straddle
    slen = partial_match(ent, delim, delim_len);
    if (slen) {
      uint32_t dlen = delim_len - slen;
      bstart = ph_buf_mem(next->buf) + next->rpos;

      if (len < slen) {
        // Not found, nor findable.
        // Consider this case:
        // |8192|1|8192|
        // Where the numbers represent ents pointing to buffers of the indicated
        // size.  If our delimiter len is 3, and the delimiter straddles three
        // entries, we won't ever find it.
        // We don't construct such a sequence today, but if we were to allow a
        // "zero copy" interface where buffers could be directly chained in,
        // this is something that might come up.
        // Whoever adds that can solve that.
        return 0;
      }

      if (memcmp(bstart, delim + dlen, slen) == 0) {
        // Found it
        clear_searchstate(q);

        searched += len + slen;
        return searched;
      }
    }

    if (q->max_record_size > 0 && delim_len <= sizeof(q->last_delim)) {
      if (q->last_overflow || ((searched + len) >= q->max_record_size)) {
        q->last_overflow = true;
        *overflow = true;

        while (true) {
          // We have overgrown our max buffer size, let's trim the fat.
          // We know we have a next buffer section, so we can safely delete
          struct ph_bufq_ent *first = PH_STAILQ_FIRST(&q->fifo);
          if (first == ent) {
            break;
          }

          searched -= (first->wpos - first->rpos);

          PH_STAILQ_REMOVE_HEAD(&q->fifo, ent);
          ph_buf_delref(first->buf);
          ph_mem_free(mt.queue_ent, first);
        }
      }
    }

    searched += len;
  }

  // No buffers in queue
  clear_searchstate(q);

  return 0;
}

ph_buf_t *ph_bufq_consume_record(ph_bufq_t *q, const char *delim,
    uint32_t delim_len)
{
  uint64_t len;
  bool overflow = 0;
  ph_buf_t *result = NULL;

  len = find_record(q, delim, delim_len, &overflow);
  if (len) {
    result = ph_bufq_consume_bytes(q, len);
  }

  errno = overflow ? EOVERFLOW : 0;
  return result;
}

ph_buf_t *ph_bufq_peek_record(ph_bufq_t *q, const char *delim,
    uint32_t delim_len)
{
  uint64_t len;
  bool overflow = 0;
  ph_buf_t *result = NULL;

  len = find_record(q, delim, delim_len, &overflow);
  if (len) {
    result = ph_bufq_peek_bytes(q, len);
  }

  errno = overflow ? EOVERFLOW : 0;
  return result;
}

bool ph_bufq_stm_write(ph_bufq_t *q, ph_stream_t *stm, uint64_t *nwrotep)
{
  struct iovec iov[16];
  uint32_t nio = 0;
  struct ph_bufq_ent *ent;
  uint64_t nwrote;
  bool res;

  PH_STAILQ_FOREACH(ent, &q->fifo, ent) {
    uint8_t *buf = ph_buf_mem(ent->buf) + ent->rpos;
    uint64_t len = ent->wpos - ent->rpos;

    if (nio >= sizeof(iov)/sizeof(iov[0])) {
      break;
    }

    if (len == 0) {
      // Shouldn't happen, but maybe we haven't gc'd?
      continue;
    }

    iov[nio].iov_base = buf;
    iov[nio].iov_len = len;
    nio++;
  }

  if (nio == 0) {
    *nwrotep = 0;
    return true;
  }

  nwrote = 0;
  res = ph_stm_writev(stm, iov, nio, &nwrote);

  if (nwrotep) {
    *nwrotep = nwrote;
  }

  if (res && nwrote > 0) {
    // Mark them as being consumed
    PH_STAILQ_FOREACH(ent, &q->fifo, ent) {
      uint64_t ate, len = ent->wpos - ent->rpos;

      if (len == 0) {
        // Nothing in here to be dealt with
        continue;
      }

      ate = MIN(len, nwrote);
      ent->rpos += ate;
      nwrote -= ate;

      if (nwrote == 0) {
        break;
      }
    }
    gc_bufq(q);
  }

  return res;
}

bool ph_bufq_stm_read(ph_bufq_t *q, ph_stream_t *stm, uint64_t *nreadp)
{
  struct ph_bufq_ent *last;
  uint64_t avail = 0, nread;
  bool res;

  last = PH_STAILQ_LAST(&q->fifo, ph_bufq_ent, ent);
  if (last) {
    avail = ph_buf_len(last->buf) - last->wpos;
  }

  if (!avail) {
    last = q_add_new_buf(q, 0);
    if (!last) {
      return false;
    }
    avail = ph_buf_len(last->buf);
  }

  res = ph_stm_read(stm, ph_buf_mem(last->buf) + last->wpos, avail, &nread);
  if (res) {
    last->wpos += nread;
  }

  if (nreadp) {
    *nreadp = nread;
  }

  return res;
}

void ph_bufq_free(ph_bufq_t *q)
{
  struct ph_bufq_ent *ent;

  while (true) {
    ent = PH_STAILQ_FIRST(&q->fifo);
    if (!ent) {
      // Nothing to prune
      break;
    }

    // We don't need this one
    PH_STAILQ_REMOVE_HEAD(&q->fifo, ent);
    ph_buf_delref(ent->buf);
    ph_mem_free(mt.queue_ent, ent);
  }

  ph_mem_free(mt.queue, q);
}


/* vim:ts=2:sw=2:et:
 */


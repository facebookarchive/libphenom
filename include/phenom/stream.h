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

#ifndef PHENOM_STREAM_H
#define PHENOM_STREAM_H

#include "phenom/defs.h"
#include "phenom/string.h"
#include "phenom/job.h"
#include <unistd.h> // For SEEK_SET

/**
 * # Streams
 *
 * libPhenom provides a portable layer over streaming IO
 */

#ifdef __cplusplus
extern "C" {
#endif

struct ph_stream;
typedef struct ph_stream ph_stream_t;
struct ph_stream_funcs;

/* size of unget buffer */
#define PH_STM_UNGET      8

/* size of default stream buffer */
#define PH_STM_BUFSIZE    8192

/* If set in flags, the stream instance must not be freed */
#define PH_STM_FLAG_ONSTACK    1

/** Represents a stream
 *
 * Streams maintain a buffer for read/write operations.
 */
struct ph_stream {
  const struct ph_stream_funcs *funcs;
  void *cookie;
  unsigned flags;
  pthread_mutex_t lock;
  // if data is in the read buffer, these are non-NULL
  unsigned char *rpos, *rend;
  // if data is in the write buffer, these are non-NULL
  unsigned char *wpos, *wend;
  unsigned char *wbase;
  // associated buffer.  It can be either used in read mode
  // or write mode, but not both
  unsigned char *buf;
  uint32_t bufsize;
  int last_err;
  ph_iomask_t need_mask;
};

/** Reads data into the provided memory buffer
 *
 * Returns true and sets nread to 0 on EOF.
 *
 * Returns false on error. errno is set accordingly, and ph_stm_errno() can
 * also be used to access that value.
 *
 * Otherwise stores the number of bytes that were read into nread
 * and returns true.
 */
bool ph_stm_read(ph_stream_t *stm, void *buf,
    uint64_t count, uint64_t *nread);

/** Requests pre-fetching of data into the read buffer
 *
 * Signals an intent to read `count` bytes of data. If the read buffer
 * already contains >= `count` bytes of data, returns true immediately.
 *
 * If there is no available buffer space (or the stream is unbuffered),
 * returns true immediately.
 *
 * Otherwise, will attempt to fill the available buffer space.
 * Returns false if there was an error performing the underlying read, sets
 * errno accordingly and makes the error code available via ph_stm_errno().
 *
 * On a successful read, returns true.
 *
 * Note that `count` is only used as a hint as to whether we need to issue
 * a read call; it doesn't influence how much data we read.  If the buffer
 * size is smaller than `count`, we can only fill the available buffer size.
 */
bool ph_stm_readahead(ph_stream_t *stm, uint64_t count);

/** Writes data from the provided memory buffer
 *
 * Returns false on error. errno is set accordingly, and ph_stm_errno() can
 * also be used to access that value.
 *
 * On success, returns true and stores the number of bytes that were written,
 * which may be less than requested due to constraints on buffer space or
 * signal interruption, into nwrote.
 */
bool ph_stm_write(ph_stream_t *stm, const void *buf,
    uint64_t count, uint64_t *nwrote);

/** Writes data via scatter-gather interface
 *
 * Returns false on error. errno is set accordingly, and ph_stm_errno() can
 * also be used to access that value.
 *
 * On success, returns true and stores the number of bytes that were written,
 * which may be less than requested due to constraints on buffer space or
 * signal interruption, into nwrote.
 */
bool ph_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrote);

/** Formatted print to a stream.
 *
 * uses ph_vprintf_core().  Returns true on success, false on error.
 * Stores the number of bytes that were written into nwrote.
 */
int ph_stm_vprintf(ph_stream_t *stm, const char *fmt, va_list ap);

/** Formatted print to a stream.
 *
 * uses ph_vprintf_core()
 */
int ph_stm_printf(ph_stream_t *stm, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;

/** Flush buffers */
bool ph_stm_flush(ph_stream_t *stm);

/** Reposition the file offset
 *
 * Equivalent to lseek(2) but with an unambiguous function signature.
 * The whence values are taken from the system
 * whence values and passed to the underlying implementation.
 *
 * The new file position is stored into *newpos unless it is NULL.
 * The return value is 0 on success, -1 on error.
 */
bool ph_stm_seek(ph_stream_t *stm, int64_t delta, int whence, uint64_t *newpos);

/** Seek back to the start of a stream
 */
static inline bool ph_stm_rewind(ph_stream_t *stm) {
  return ph_stm_seek(stm, 0, SEEK_SET, NULL);
}

/** Read data from src and write to dest
 *
 * Reads up to the specified number of bytes from `src` and writes
 * them to `dest`.  If `num_bytes` is the special value
 * `PH_STREAM_READ_ALL` then all remaining data from `src` will
 * be read.
 *
 * Returns `true` on success, which is considered to be that no IO
 * errors except EOF when reading from `src` were encountered, or
 * `false` otherwise.
 *
 * Sets `*nread` to the number of bytes read from `src`, unless
 * `nread` is NULL.
 *
 * Sets `*nwrote` to the number of bytes successfully written to `dest`,
 * unless `nwrote` is NULL.
 */
bool ph_stm_copy(ph_stream_t *src, ph_stream_t *dest,
    uint64_t num_bytes, uint64_t *nread, uint64_t *nwrote);

#define PH_STREAM_READ_ALL UINT64_MAX

/** Close the stream and release resources
 *
 * Requests that the stream be closed.  On success, the stream handle
 * is invalidated and should no longer be used.
 */
bool ph_stm_close(ph_stream_t *stm);

/** Returns the errno value from the last error
 *
 * Each IO operation clears the stored error to 0, setting it again
 * only in case of an IO failure.  Calling ph_stm_errno() will thus
 * return 0 if there was no prior error, or the errno from the last
 * error condition.
 */
static inline int ph_stm_errno(ph_stream_t *stm) {
  return stm->last_err;
}

/** Construct a stream around a pre-existing file descriptor */
ph_stream_t *ph_stm_fd_open(int fd, int sflags, uint32_t bufsize);

void ph_stm_lock(ph_stream_t *stm);
void ph_stm_unlock(ph_stream_t *stm);

/** Open a file stream
 *
 * oflags are passed to the open(2) syscall.
 */
ph_stream_t *ph_stm_file_open(const char *filename, int oflags, int mode);

/** Open a stream over a string object
 *
 * The returned string will start at position 0; reads will read the
 * string buffer, hitting EOF when the end of the string is reached.
 *
 * Writes will replace string bytes at the current position.  If a
 * write would overflow the string, the overflown portion will be
 * appended to the string, provided that the underlying append operation
 * succeeds.
 *
 * The stream will add and retain its own reference to the string
 * object that will be released when the stream is closed.
 */
ph_stream_t *ph_stm_string_open(ph_string_t *str);

/**
 * # Implementation Details
 *
 * If you're simply consuming streams, you can stop reading now.
 */

/** Defines a stream implementation.
 *
 * If any of these return false, it indicates an error.
 * The implementation must set stm->last_err to the corresponding
 * errno value in that case (and only in the failure case).
 */
struct ph_stream_funcs {
  bool (*close)(ph_stream_t *stm);
  bool (*readv)(ph_stream_t *stm, const struct iovec *iov,
      int iovcnt, uint64_t *nread);
  bool (*writev)(ph_stream_t *stm, const struct iovec *iov,
      int iovcnt, uint64_t *nwrote);
  bool (*seek)(ph_stream_t *stm, int64_t delta,
      int whence, uint64_t *newpos);
};

/** Construct a stream.
 * If bufsize is 0, the stream will be unbuffered
 */
ph_stream_t *ph_stm_make(const struct ph_stream_funcs *funcs,
    void *cookie, int sflags, uint32_t bufsize);

/** Destruct a stream.
 * Does not call close, forces destruction of the stm.
 */
void ph_stm_destroy(ph_stream_t *stm);

/* functions that operate on a file descriptor */
extern struct ph_stream_funcs ph_stm_funcs_fd;


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


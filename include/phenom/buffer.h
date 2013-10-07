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

#ifndef PHENOM_BUFFER_H
#define PHENOM_BUFFER_H

/**
 * # Buffers
 *
 * The libPhenom buffer API allows for fixed-size buffers to be used reasonably
 * efficiently in an application.  The API provides a couple of benefits:
 *
 * * Slices can be made over a region of another buffer.  The slice allows
 *   for the region to be referenced without copying, while still safely
 *   managing the underlying storage
 * * The object representation of a string buffer means that we can avoid
 *   string interning costs when integrating with runtimes such as Lua
 * * Discontiguous buffers can be used to accumulate data.  The fixed size
 *   of these buffers helps to reduce heap fragmentation.  An API is provided
 *   to locate and slice (or duplicate if needed) a delimited record (such
 *   as CRLF delimited lines)
 */

#include "phenom/defs.h"
#include "phenom/stream.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_buf;
typedef struct ph_buf ph_buf_t;

struct ph_bufq;
typedef struct ph_bufq ph_bufq_t;

/** Create a new buffer
 *
 * The buffer will be at least the size that you requested; the implementation
 * may decide to round your allocation up to one of a set of common buffer
 * sizes.
 *
 * If you specify 0 for the size, a default buffer size will be selected
 * for you.
 *
 * The contents of the buffer are undefined.
 *
 * Returns a reference to the buffer.  When the final reference is released,
 * the storage is reclaimed.
 *
 * Use ph_buf_delref() to release a reference to a buffer.
 */
ph_buf_t *ph_buf_new(uint64_t size);

/** Add a reference to a buffer */
void ph_buf_addref(ph_buf_t *buf);

/** Release a reference to a buffer */
void ph_buf_delref(ph_buf_t *buf);

/** Create a slice over a buffer
 *
 * Creates a new buffer object that references a region in another buffer.
 *
 * The slice takes a reference on the other buffer; when the slice is
 * destroyed, it will release that reference.
 *
 * Operating on the slice operates on the specified region of the original
 * buffer.
 *
 * If len == 0, len is interpreted as the length that covers the region from
 * the start to the end of the buffer.
 */
ph_buf_t *ph_buf_slice(ph_buf_t *buf, uint64_t start, uint64_t len);

/** Copy data from one buffer to another
 *
 * Copies the specified range of bytes from the source buffer to the
 * destination buffer at the specified offset.
 *
 * The source and destination buffer may be the same object; the memory
 * ranges may overlap.
 *
 * If either the source or destination range is out of bounds, no copying
 * takes place and this function will return false.
 *
 * Otherwise, returns true
 */
bool ph_buf_copy(ph_buf_t *src, ph_buf_t *dest, uint64_t start, uint64_t len,
    uint64_t dest_start);

bool ph_buf_copy_mem(ph_buf_t *dest, const void *mem,
    uint64_t len, uint64_t dest_start);

/** Fills a buffer region with a byte value
 *
 * Logically equivalent to memset(3), but operates on a range in a buffer
 */
bool ph_buf_set(ph_buf_t *buf, int value, uint64_t start, uint64_t len);

/** Returns the size of the buffer
 *
 * Return the number of bytes of available storage in the buffer; this is
 * the size of the allocated storage space.  The buffer API doesn't track
 * whether bytes are "used" (there is no cursor, since slices make this
 * awkward to track), so this is simply the raw number of bytes in the
 * memory backing the buffer object.
 */
uint64_t ph_buf_len(ph_buf_t *buf);

/** Returns a pointer to the start of the buffer memory
 *
 * This is the raw buffer memory; you are responsible for constraining your
 * read/writes to this region within the length specified by ph_buf_len().
 */
uint8_t *ph_buf_mem(ph_buf_t *buf);

/** Concatenate a sequence of buffers together
 *
 * Allocates a buffer that is large enough to hold the total number of
 * bytes specified, then copies data from the sequence of buffers into
 * this new buffer.
 *
 * This is useful when matching a record across a series of buffers and
 * then wanting to pass the data to code that requires a contiguous memory
 * buffer.
 *
 * If length == 0, the function will query the total length of the buffers
 * and use that as the length.
 *
 * `first_offset` specifies the number of bytes to skip in the first buffer.
 */
ph_buf_t *ph_buf_concat(uint64_t length, uint32_t num_bufs,
    ph_buf_t **buffers, uint64_t first_offset);

/** Create a new buffer queue
 *
 * A buffer queue allows buffering of buffers in a FIFO manner.
 * This is useful to implement a segmented read or write buffer with an
 * upper bound on buffer size.
 *
 * A freshly created buffer queued is populated with an initial ph_buf_t
 * with the default size.
 */
ph_bufq_t *ph_bufq_new(uint64_t max_size);

/** Destroy a buffer queue
 *
 * Releases all of its resources
 */
void ph_bufq_free(ph_bufq_t *q);

/** Accumulate data into a buffer queue
 *
 * Copies the data from the provided raw buffer pointer into the buffer
 * queue.  Attempts to top off the "last" logical ph_buf_t in the queue.
 * If the data being added won't fit in the buffer, a new buffer is allocated
 * and appended.
 *
 * If the data to be appended would exceed the maximum size of the buffer
 * queue, only the data up to the size limit will be added.
 *
 * Returns PH_OK on success (which may mean partial success), or an error
 * code on failure (such as failure to allocate buffers).
 *
 * Populates the number of bytes that were consumed in the added_bytes
 * parameter.
 */
ph_result_t ph_bufq_append(ph_bufq_t *q, const void *buf, uint64_t len,
    uint64_t *added_bytes);

/** Attempts to de-queue data from a buffer queue
 *
 * If the requested number of bytes are available in the queue, concatenate
 * them into a contiguous buffer and return that buffer.
 *
 * Those bytes are considered read and the queue will
 * release its reference on the associated buffers, potentially releasing
 * memory if those references fall to zero.
 *
 * If the requested number of bytes are not present, returns NULL.
 */
ph_buf_t *ph_bufq_consume_bytes(ph_bufq_t *q, uint64_t len);

/** Peek at data in the buffer queue
 *
 * If the requested number of bytes are available in the queue, concatenate
 * them into a contiguous buffer and return that buffer.
 *
 * The state of the queue is left unchanged.
 *
 * If the requested number of bytes are not present, returns NULL.
 */
ph_buf_t *ph_bufq_peek_bytes(ph_bufq_t *q, uint64_t len);

/** Attempts to de-queue a record from a buffer queue
 *
 * Searches the buffer queue until it finds the delimiter text.
 * If the delimiter text is not found, returns NULL.
 *
 * If the delimiter text is found, constructs a buffer to reference
 * that memory region and dequeues it as though ph_bufq_consume_bytes()
 * was called with the appropriate length to the end of the delimiter text.
 *
 * The delimiter is included in the returned buffer.
 */
ph_buf_t *ph_bufq_consume_record(ph_bufq_t *q, const char *delim,
    uint32_t delim_len);

/** Attempts to peek at a record from a buffer queue
 *
 * Searches the buffer queue until it finds the delimiter text.
 * If the delimiter text is not found, returns NULL.
 *
 * If the delimiter text is found, constructs a buffer to reference
 * that memory region and peeks at it as though ph_bufq_peek_bytes()
 * was called with the appropriate length to the end of the delimiter text.
 *
 * The delimiter is included in the returned buffer.
 */
ph_buf_t *ph_bufq_peek_record(ph_bufq_t *q, const char *delim,
    uint32_t delim_len);

/** Attempts to consume data data from a queue and write to a stream
 *
 * Constructs an iovec representing the queued buffers up to an internal
 * limit of iovec elements, then calls the writev implementation of the stream,
 * ignoring any buffering that may be present on the stream.  It is recommended
 * that you use an unbuffered stream, but if you decide not to, you are responsible
 * for ensuring that the stream has been flushed prior to calling this function.
 *
 * Any successfully written bytes are considered consumed and the associated
 * storage may be released.
 *
 * Returns true on success (which will likely be a partial write) and sets
 * *nwrote to the number of bytes that were consumed.
 *
 * Returns false on failure; you may call ph_stm_errno() to determine what
 * went wrong.
 */
bool ph_bufq_stm_write(ph_bufq_t *q, ph_stream_t *stm, uint64_t *nwrote);

/** Attempts to read data from a stream and accumulate it in bufq
 *
 * If the bufq has a partially filled buffer at the tail, ph_stm_read() will
 * be invoked to attempt to fill that space.
 *
 * If not bufs with space are present in the bufq, a default buffer will be
 * added and ph_stm_read() will be invoked to fill that space.
 *
 * This function will invoke ph_stm_read() at most once per call.
 */
bool ph_bufq_stm_read(ph_bufq_t *q, ph_stream_t *stm, uint64_t *nread);

/** Returns the number of bytes to be consumed */
uint64_t ph_bufq_len(ph_bufq_t *q);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */


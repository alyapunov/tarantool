/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2022, Tarantool AUTHORS, please see AUTHORS file.
 */
#include "prbuf.h"
#include "bit/bit.h"

/**
 * Partitioned ring buffer. Each entry stores size before user data.
 * So the typical buffer looks like:
 * HEADER uint32 DATA uint32 DATA ...
 *
 * We have to store offsets to be able to restore buffer (including
 * all metadata) from raw pointer. Otherwise it is impossible to point
 * out where head/tail are located.
 */
struct PACKED prbuf_header {
	/**
	 * Buffer's data layout can be changed in the future, so for the sake
	 * of proper recovery of the buffer we store its version.
	 */
	uint32_t version;
	/** Total size of buffer (including header). */
	uint32_t size;
	/**
	 * Offset of the oldest entry - it is the first candidate to be
	 * overwritten. Note that in contrast to iterator/entry - this offset
	 * is calculated to the first byte of entry (i.e. header containing
	 * size of entry).
	 * */
	uint32_t offset_begin;
	/** Offset of the next byte after the last (written) record. */
	uint32_t offset_end;
};

/**
 * Structure representing record stored in the buffer so it has the same
 * memory layout.
 */
struct prbuf_record {
	/** Size of data. */
	uint32_t size;
	/** Data. */
	char data[];
};

/**
 * Current prbuf implementation version. Must be bumped in case storage
 * format is changed.
 */
static const uint32_t prbuf_version = 0;

/**
 * prbuf is assumed to store all metadata in little-endian format. Beware
 * when decoding its content. Size overhead per store operation is 4 bytes;
 * moreover not the whole space of the buffer can be used since we do not
 * wrap entry if it doesn't fit til the buffer's end.
 *
 * There are several assumptions regarding the buffer:
 * - Buffer always contains at least one element;
 * - The end of the buffer (in the linear sense) contains "end mark";
 * - The minimal size of the buffer is restricted;
 * - Iteration direction - from the oldest entry to the newest;
 */

/** A mark of unused space in the buffer: trash is located after this point. */
static const uint32_t prbuf_end_position = (uint32_t)(-1);

/** Before storing a data in the buffer we place its size (i.e. header). */
static const size_t entry_meta_size = sizeof(uint32_t);

static struct prbuf_entry invalid_entry =
	{ .size = (uint32_t)(-1), .ptr = NULL };

/** Real size of allocation is (data size + record's header). */
static uint32_t
prbuf_record_alloc_size(size_t size)
{
	return size + entry_meta_size;
}

/** Returns pointer to the next byte after end of given buffer. */
static char *
prbuf_linear_end(struct prbuf *buf)
{
	return (char *) buf->header + buf->header->size;
}

/** Returns pointer to the first writable byte of given buffer. */
static char *
prbuf_linear_begin(struct prbuf *buf)
{
	return (char *) buf->header + sizeof(struct prbuf_header);
}

/** Returns pointer to the next byte after the last written record. */
static char *
prbuf_current_raw(struct prbuf *buf)
{
	return prbuf_linear_begin(buf) + buf->header->offset_end;
}

static struct prbuf_record *
prbuf_current_record(struct prbuf *buf)
{
	return (struct prbuf_record *) prbuf_current_raw(buf);
}

/** Returns first (in historical sense) record. */
static struct prbuf_record *
prbuf_first_record(struct prbuf *buf)
{
	assert(buf->header->offset_begin != prbuf_end_position);
	char *first_ptr = prbuf_linear_begin(buf) + buf->header->offset_begin;
	return (struct prbuf_record *) first_ptr;
}

/** Calculate offset from the buffer's start to the given entry. */
static uint32_t
prbuf_record_offset(struct prbuf *buf, struct prbuf_record *record)
{
	assert((char *) record >= prbuf_linear_begin(buf));
	return (uint32_t) ((char *) record - prbuf_linear_begin(buf));
}

/** Returns true in case buffer has at least @a size bytes until its linear end. */
static bool
prbuf_has_before_end(struct prbuf *buf, uint32_t size)
{
	assert(prbuf_linear_end(buf) >= prbuf_current_raw(buf));
	if ((uint32_t) (prbuf_linear_end(buf) - prbuf_current_raw(buf)) >= size)
		return true;
	return false;
}

void
prbuf_create(struct prbuf *buf, void *mem, size_t size)
{
	assert(size > sizeof(struct prbuf));
	buf->header = (struct prbuf_header *) mem;
	buf->header->offset_end = 0;
	buf->header->offset_begin = 0;
	buf->header->version = prbuf_version;
	buf->header->size = size;

	uint32_t available_space = size - sizeof(struct prbuf_header);
#ifndef NDEBUG
	memset(prbuf_linear_begin(buf), '#', available_space);
#endif
	/*
	 * Place single entry occupying whole space. It's done just for
	 * the sake of convenience.
	 */
	char *begin = prbuf_current_raw(buf);
	store_u32(begin, available_space - entry_meta_size);
	buf->header->offset_end = available_space;
}

/**
 * Verify that prbuf remains in the consistent state: header is valid and
 * all entries have readable sizes.
 */
static bool
prbuf_check(struct prbuf *buf)
{
	if (buf->header->version != prbuf_version)
		return false;
	struct prbuf_iterator iter;
	struct prbuf_entry res;
	prbuf_iterator_create(buf, &iter);
	uint32_t total_size = 0;
	while (prbuf_iterator_next(&iter, &res) == 0 &&
	       ! prbuf_entry_is_invalid(&res))
		total_size += res.size;
	return (total_size <= buf->header->size);
}

int
prbuf_open(struct prbuf *buf, void *mem)
{
	buf->header = (struct prbuf_header *) mem;
	if (! prbuf_check(buf))
		return -1;
	return 0;
}

static struct prbuf_record *
prbuf_skip_record(struct prbuf *buf, struct prbuf_record *current,
		  ssize_t to_store)
{
	assert(to_store > 0);
	assert(to_store <= buf->header->size);
	struct prbuf_iterator iter = {
		.buf = buf,
		.current = current,
	};
	struct prbuf_entry res;
	while (to_store > 0) {
		assert(iter.current->size != prbuf_end_position);
		assert(iter.current->size != 0);
		to_store -= prbuf_record_alloc_size(iter.current->size);
		if (prbuf_iterator_next(&iter, &res) != 0 &&
		    ! prbuf_entry_is_invalid(&res)) {
			prbuf_iterator_create(buf, &iter);
			prbuf_iterator_next(&iter, &res);
		}
	}
	return iter.current;
}

/** Place special mark at the end of buffer to avoid out-of-bound access. */
static void
prbuf_set_end_position(struct prbuf *buf)
{
	if (prbuf_has_before_end(buf, entry_meta_size))
		prbuf_current_record(buf)->size = prbuf_end_position;
}

/** Store entry's size. */
static void *
prbuf_prepare_record(struct prbuf_record *record, size_t size)
{
	record->size = size;
	return record->data;
}

void *
prbuf_prepare(struct prbuf *buf, size_t size)
{
	assert(size > 0);
	uint32_t alloc_size = prbuf_record_alloc_size(size);
	if (alloc_size > (buf->header->size - sizeof(struct prbuf_header)))
		return NULL;
	if (prbuf_has_before_end(buf, alloc_size)) {
		/* Head points to the byte right after the last written entry. */
		char *head = prbuf_current_raw(buf);
		struct prbuf_record *next = prbuf_first_record(buf);
		/* free_space can be overflowed - it's completely OK. */
		uint32_t free_space = (char *) next - head;
		/*
		 * We can safely write entry in case it won't overwrite
		 * anything. Either trash space between two entries
		 * is large enough or the next entry to be overwritten
		 * is located at the start of the buffer.
		 */
		if (free_space < alloc_size) {
			struct prbuf_record *next_overwritten =
				prbuf_skip_record(buf, next, alloc_size);
			buf->header->offset_begin =
				  prbuf_record_offset(buf, next_overwritten);
		}
		return prbuf_prepare_record((struct prbuf_record *) head, size);
	}
	/*
	 * Data doesn't fit till the end of buffer, so we'll put the entry
	 * at the buffer's start. Moreover, we should mark the last entry
	 * (in linear sense) to avoid oud-of-bound access while parsing buffer
	 * (after this mark trash is stored so we can't process further).
	 */
	prbuf_set_end_position(buf);
	struct prbuf_record *head =
		(struct prbuf_record *)  prbuf_linear_begin(buf);
	struct prbuf_record *next_overwritten =
		prbuf_skip_record(buf, head, alloc_size);
	buf->header->offset_begin = prbuf_record_offset(buf, next_overwritten);
	return prbuf_prepare_record(head, size);
}

void
prbuf_commit(struct prbuf *buf)
{
	if (prbuf_has_before_end(buf, entry_meta_size)) {
		struct prbuf_record *last = prbuf_current_record(buf);
		if (prbuf_has_before_end(buf, last->size)) {
			buf->header->offset_end +=
				prbuf_record_alloc_size(last->size);
			return;
		}
	}
	struct prbuf_record *last =
		(struct prbuf_record *) prbuf_linear_begin(buf);
	buf->header->offset_end = prbuf_record_alloc_size(last->size);
}

void
prbuf_iterator_create(struct prbuf *buf, struct prbuf_iterator *iter)
{
	iter->buf = buf;
	iter->current = NULL;
}

int
prbuf_iterator_next(struct prbuf_iterator *iter, struct prbuf_entry *result)
{
	struct prbuf *buf = iter->buf;
	*result = invalid_entry;
	if (iter->current == NULL) {
		iter->current = prbuf_first_record(buf);
		result->size = iter->current->size;
		result->ptr = iter->current->data;
		assert(iter->current->size <= buf->header->size);
		return 0;
	}

	if (iter->current->size > (buf->header->size - sizeof(struct prbuf_header)))
		return -1;
	assert((char *)iter->current >= prbuf_linear_begin(buf));
	char *next_record_ptr = iter->current->data + iter->current->size;
	if (next_record_ptr == prbuf_current_raw(buf))
		return 0;
	assert(prbuf_linear_end(buf) >= next_record_ptr);

	struct prbuf_record *next = (struct prbuf_record *) next_record_ptr;
	if ((uint32_t)(prbuf_linear_end(buf) - next_record_ptr) < entry_meta_size)
		next = (struct prbuf_record *) prbuf_linear_begin(buf);
	else if (next->size == prbuf_end_position)
		next = (struct prbuf_record *)  prbuf_linear_begin(buf);

	iter->current = next;
	result->size = next->size;
	result->ptr = next->data;
	return 0;
}

bool
prbuf_entry_is_invalid(struct prbuf_entry *entry)
{
	return entry->size == invalid_entry.size &&
	       entry->ptr == invalid_entry.ptr;
}

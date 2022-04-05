/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2022, Tarantool AUTHORS, please see AUTHORS file.
 */
#include "prbuf.h"

#include <assert.h>
#include <string.h>

/**
 * Partitioned ring buffer. Each record stores size before user data.
 * So the typical buffer looks like:
 * HEADER....... uint32 DATA uint32 DATA ...
 *
 * We have to store offsets to be able to restore buffer (including
 * all metadata) from raw pointer. Otherwise it is impossible to point
 * out where head/tail are located. Offsets are measured from the beginning
 * of entire memory block (that is the first byte of header). For example:
 * HEADER....... uint32 DATA uint32 DATA
 * <---begin---->
 * <--------------end------------------>
 *
 * All records are aligned, so there can be a small paddings between records.
 * In the end of the buffer there can be a fake record. It appears when newly
 * added record does not fit the space from current write position to the end
 * of buffer, so we have to fill remaining space with something correct but
 * distinguishable (for iterator) from normal record.
 */
struct prbuf_header {
	/**
	 * Buffer's data layout can be changed in the future, so for the sake
	 * of proper recovery of the buffer we store its version.
	 */
	uint32_t version;
	/** Total size of buffer (including header). */
	uint32_t size;
	/** Offset of the first (oldest) record. */
	uint32_t begin;
	/** Offset of the next byte after the last (written) record. */
	uint32_t end;
};

static const uint32_t BASE_OFFSET = sizeof(struct prbuf_header);
static const uint32_t PRBUF_FAKE = 1u << 31;
static const uint32_t PRBUF_SIZE = UINT32_MAX ^ PRBUF_FAKE;

/**
 * Structure representing record stored in the buffer so it has the same
 * memory layout.
 */
struct prbuf_record {
	/** Size of data ORed with flags. */
	uint32_t flag_size;
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
 * - The minimal size of the buffer is restricted;
 * - Iteration direction - from the oldest entry to the newest;
 */

void
prbuf_create(struct prbuf *buf, void *mem, size_t size)
{
#ifndef NDEBUG
	memset(mem, '#', size);
#endif
	assert(size > BASE_OFFSET);
	assert(size < PRBUF_SIZE);
	buf->header = (struct prbuf_header *) mem;
	buf->header->version = prbuf_version;
	buf->header->size = size;
	buf->header->begin = BASE_OFFSET;
	buf->header->end = BASE_OFFSET;
}

/** Get record by offset. */
static struct prbuf_record *
prbuf_get_record(struct prbuf_header *h, uint32_t offset)
{
	return (struct prbuf_record *) ((char *) h + offset);
}

/**
 * Total size of a record, including struct prbuf_record header, rounded up to
 * fulfill record alignment. You may pass flags withing size argument, they
 * will be ignored.
 */
static uint32_t
prbuf_record_size(uint32_t size)
{
	const uint32_t jump = offsetof(struct prbuf_record, data[0]);
	const uint32_t align = _Alignof(struct prbuf_record);
	const uint32_t mask = PRBUF_SIZE & ~(align - 1);
	return (size + jump + align - 1) & mask;
}

int
prbuf_open(struct prbuf *buf, void *mem, size_t size)
{
	buf->header = mem;
	struct prbuf_header *h = buf->header;
	if (size <= BASE_OFFSET || size >= PRBUF_SIZE)
		return -1;
	if (h->version != prbuf_version)
		return -1;
	if (h->size != size)
		return -1;
	if (h->begin < BASE_OFFSET || h->begin >= h->size)
		return -1;
	if (h->end < BASE_OFFSET || h->end >= h->size)
		return -1;

	/* Check records. */
	if (h->begin == BASE_OFFSET && h->end == BASE_OFFSET)
		return 0; /* The one and only case with no records. */

	uint32_t current = h->begin;
	while (current >= h->end) {
		struct prbuf_record *rec = prbuf_get_record(h, current);
		uint32_t rec_size = prbuf_record_size(rec->flag_size);
		if (rec_size > h->size - current)
			return -1;
		if ((rec->flag_size & PRBUF_FAKE) &&
		    (rec_size != h->size - current))
			return -1;
		if (rec_size == h->size - current)
			current = BASE_OFFSET;
		else
			current += rec_size;
	}

	while (current < h->end) {
		struct prbuf_record *rec = prbuf_get_record(h, current);
		uint32_t rec_size = prbuf_record_size(rec->flag_size);
		if (rec->flag_size & PRBUF_FAKE)
			return -1;
		if (rec_size > h->end - current)
			return -1;
		current += rec_size;
	}

	return 0;
}

/** Remove the first record from list. */
static void
prbuf_drop_record(struct prbuf_header *h)
{
	assert(h->begin != h->end);
	struct prbuf_record *rec = prbuf_get_record(h, h->begin);
	uint32_t pos = h->begin + prbuf_record_size(rec->flag_size);
	assert(pos <= h->size);
	if (pos == h->size)
		pos = BASE_OFFSET;
	h->begin = pos;
}

/** Initialize fake record in the end of record list. */
static void
prbuf_prepare_fake(struct prbuf_header *h)
{
	assert(h->begin <= h->end);
	assert(h->size - h->end >= sizeof(struct prbuf_record));
	uint32_t fake_size = h->size - h->end - sizeof(struct prbuf_record);
	struct prbuf_record *fake = prbuf_get_record(h, h->end);
	fake->flag_size = fake_size | PRBUF_FAKE;
}

/** Store entry's size and return pointer to data. */
static void *
prbuf_prepare_record(struct prbuf_header *h, size_t size)
{
	struct prbuf_record *record = prbuf_get_record(h, h->end);
	record->flag_size = size;
	return record->data;
}

void *
prbuf_prepare(struct prbuf *buf, uint32_t size)
{
	assert(size > 0);
	struct prbuf_header *h = buf->header;
	uint32_t alloc_size = prbuf_record_size(size);
	if (BASE_OFFSET + alloc_size > h->size)
		return NULL;

	/* Try to allocate from current end. */
	while (h->begin > h->end && h->end + alloc_size > h->begin)
		prbuf_drop_record(h);
	if (h->end + alloc_size <= h->size)
		return prbuf_prepare_record(h, size);

	/* Fall to the beginning of buffer. */
	assert(h->begin == BASE_OFFSET);
	prbuf_drop_record(h);
	if (h->end != h->size)
		prbuf_prepare_fake(h);
	h->end = BASE_OFFSET;

	/* Allocate in the beginning of buffer. */
	while (h->begin > h->end && h->end + alloc_size > h->begin)
		prbuf_drop_record(h);
	return prbuf_prepare_record(h, size);
}

void
prbuf_commit(struct prbuf *buf)
{
	struct prbuf_header *h = buf->header;
	struct prbuf_record *rec = prbuf_get_record(h, h->end);
	h->end += prbuf_record_size(rec->flag_size);
	assert(h->end <= h->size);
}

void
prbuf_iterator_create(struct prbuf *buf, struct prbuf_iterator *iter)
{
	iter->header = buf->header;
	iter->current = 0;
}

int
prbuf_iterator_next(struct prbuf_iterator *itr, char **data, uint32_t *size)
{
	struct prbuf_header *h = itr->header;
	struct prbuf_record *rec;
	if (itr->current == 0) {
		if (h->begin == BASE_OFFSET && h->end == BASE_OFFSET)
			return -1; /* Empty buffer. */
		itr->current = h->begin;
	} else if (itr->current == h->end) {
		return -1; /* No more records. */
	}

again:
	rec = prbuf_get_record(h, itr->current);
	itr->current += prbuf_record_size(rec->flag_size);
	assert(itr->current <= h->size);
	if (itr->current == h->size)
		itr->current = BASE_OFFSET;
	if (rec->flag_size & PRBUF_FAKE)
		goto again; /* Skip fake. */

	*data = rec->data;
	*size = rec->flag_size;
	return 0;
}

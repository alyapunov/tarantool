#pragma once
/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2022, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Defined in prbuf.c */
struct prbuf_header;
struct prbuf_record;

/**
 * prbuf stands for partitioned ring buffer. It is designed in the way that
 * buffer can be recovered from raw memory.
 */
struct prbuf {
	/**
	 * Header contains all buffer's metadata. Header is stored in scope
	 * of provided for buffer memory. So it's possible to restore all
	 * buffer's data from raw pointer.
	 */
	struct prbuf_header *header;
};

struct prbuf_iterator {
	/** Iterator is related to this buffer. */
	struct prbuf_header *header;
	/** Iterator is positioned to this entry. */
	uint32_t current;
};

/**
 * Create an empry prbuf entry. Metadata for the buffer is allocated in the
 * provided @mem, so in fact the capacity of the buffer is less than @a size.
 * Destructor for buffer is not provided.
 */
void
prbuf_create(struct prbuf *buf, void *mem, size_t size);

/**
 * Consider @a mem containing valid prbuf structure. Parse metadata and
 * verify the content of the buffer. In case current buffer version does not
 * match given one or buffer contains spoiled entry - return -1.
 */
int
prbuf_open(struct prbuf *buf, void *mem, size_t size);

/**
 * Returns pointer to memory chunk sizeof @a size.
 * Note that without further prbuf_commit() call this function may return
 * the same chunk twice.
 */
void *
prbuf_prepare(struct prbuf *buf, uint32_t size);

/** Commits the last prepared memory chunk. */
void
prbuf_commit(struct prbuf *buf);

/** Create an iterator. */
void
prbuf_iterator_create(struct prbuf *buf, struct prbuf_iterator *iter);

/**
 * Gets an entry and moves the iterator to the next position.
 * returns 0 on success, -1 if there are no more entries.
 */
int
prbuf_iterator_next(struct prbuf_iterator *iter, char **data, uint32_t *size);

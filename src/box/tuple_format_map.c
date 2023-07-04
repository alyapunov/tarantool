/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2023, Tarantool AUTHORS, please see AUTHORS file.
 */

#include "box/tuple_format_map.h"
#include "box/tuple.h"

#include "msgpuck/msgpuck.h"

#include "mpstream/mpstream.h"

#include "small/obuf.h"

/**
 * Add a tuple format to the tuple format and reference the format.
 */
static void
tuple_format_map_add_format_impl(struct tuple_format_map *map,
				 uint16_t format_id,
				 struct tuple_format *format)
{
	tuple_format_ref(format);
	struct mh_i32ptr_node_t node = {
		.key = format_id,
		.val = format,
	};
	if (map->hash_table == NULL) {
		if (map->cache_last_index < TUPLE_FORMAT_MAP_CACHE_SIZE - 1) {
			map->cache[++map->cache_last_index] = node;
			return;
		}
		map->hash_table = mh_i32ptr_new();
		for (size_t i = 0; i < TUPLE_FORMAT_MAP_CACHE_SIZE; ++i)
			mh_i32ptr_put(map->hash_table, &map->cache[i], NULL,
				      NULL);
	}
	mh_i32ptr_put(map->hash_table, &node, NULL, NULL);
	map->cache_last_index =
		(map->cache_last_index + 1) % TUPLE_FORMAT_MAP_CACHE_SIZE;
	map->cache[map->cache_last_index] = node;
}

void
tuple_format_map_create_empty(struct tuple_format_map *map)
{
	map->cache_last_index = -1;
	map->hash_table = NULL;
}

int
tuple_format_map_create_from_mp(struct tuple_format_map *map, const char *data)
{
	tuple_format_map_create_empty(map);
	if (mp_typeof(*data) != MP_MAP)
		return -1;
	uint32_t map_sz = mp_decode_map(&data);
	for (uint32_t i = 0; i < map_sz; ++i) {
		if (mp_typeof(*data) != MP_UINT) {
			return -1;
		}
		uint16_t format_id = mp_decode_uint(&data);
		if (mp_typeof(*data) != MP_ARRAY) {
			return -1;
		}
		const char *format_data = data;
		mp_next(&data);
		size_t format_data_len = data - format_data;
		struct tuple_format *format =
			runtime_tuple_format_new(format_data, format_data_len,
						 /*names_only=*/true);
		if (format == NULL)
			return -1;
		tuple_format_map_add_format_impl(map, format_id, format);
	}
	return 0;
}

void
tuple_format_map_destroy(struct tuple_format_map *map)
{
	if (map->hash_table == NULL) {
		for (ssize_t i = 0; i < map->cache_last_index + 1; ++i)
			tuple_format_unref(map->cache[i].val);
	} else {
		mh_int_t k;
		struct mh_i32ptr_t *h = map->hash_table;
		mh_foreach(h, k)
			tuple_format_unref(mh_i32ptr_node(h, k)->val);
		mh_i32ptr_delete(map->hash_table);
	}
}

void
tuple_format_map_move(struct tuple_format_map *dst,
		      struct tuple_format_map *src)
{
	memcpy(dst, src, sizeof(*dst));
	src->cache_last_index = -1;
	src->hash_table = NULL;
}

void
tuple_format_map_add_format(struct tuple_format_map *map, uint16_t format_id)
{
	struct tuple_format *format = tuple_format_by_id(format_id);
	tuple_format_map_add_format_impl(map, format_id, format);
}

void
tuple_format_map_to_mpstream(struct tuple_format_map *map,
			     struct mpstream *stream)
{
	size_t format_id_count;
	if (map->cache_last_index == -1) {
		assert(map->hash_table == NULL);
		format_id_count = 0;
	} else if (map->hash_table == NULL) {
		format_id_count = map->cache_last_index + 1;
	} else {
		format_id_count = mh_size(map->hash_table);
	}
	mpstream_encode_map(stream, format_id_count);
	if (map->hash_table == NULL) {
		for (size_t i = 0; i < format_id_count; ++i)
			tuple_format_to_mpstream(map->cache[i].val, stream);
	} else {
		mh_int_t k;
		struct mh_i32ptr_t *h = map->hash_table;
		mh_foreach(h, k)
			tuple_format_to_mpstream(mh_i32ptr_node(h, k)->val,
						 stream);
	}
}

struct tuple_format *
tuple_format_map_find(struct tuple_format_map *map, uint16_t format_id)
{
	if (map->cache_last_index == -1) {
		assert(map->hash_table == NULL);
		return NULL;
	}
	size_t cache_size = map->hash_table == NULL ?
			    (size_t)(map->cache_last_index + 1) :
			    TUPLE_FORMAT_MAP_CACHE_SIZE;
	for (size_t i = 0; i < cache_size; ++i) {
		if (map->cache[i].key == format_id)
			return map->cache[i].val;
	}
	if (map->hash_table == NULL)
		return NULL;
	mh_int_t k = mh_i32ptr_find(map->hash_table, format_id, NULL);
	if (k == mh_end(map->hash_table))
		return NULL;
	map->cache_last_index =
		(map->cache_last_index + 1) % TUPLE_FORMAT_MAP_CACHE_SIZE;
	map->cache[map->cache_last_index] = *mh_i32ptr_node(map->hash_table, k);
	return map->cache[map->cache_last_index].val;
}

/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tuple_constraint;
struct tuple;
struct space;

/**
 * Initialize @a constraint assuming that it is a foreign key.
 */
int
tuple_constraint_fkey_init(struct tuple_constraint *constraint,
			   struct space *space, int32_t field_no);


/**
 * Check that @a tuple, that can potentially be a foreign tuple due to
 * @a constraint, is allowed to be deleted.
 * return 0: the tuple can be deleted, data integrity is safe.
 * return -1: the tuple can't be deleted, diag is set.
 */
int
tuple_constraint_fkey_check_delete(const struct tuple_constraint *constraint,
				   struct tuple *tuple);

#ifdef __cplusplus
} /* extern "C" */
#endif

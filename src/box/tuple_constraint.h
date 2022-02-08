/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#include "tuple_constraint_def.h"
#include "func_cache.h"
#include "space_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tuple_constraint;
struct tuple_field;
struct tuple_format;
struct space;
struct tuple_format;

/**
 * Type of constraint check function.
 * The check can be done for fields (@a field != NULL, mp_data points to field
 * msgpack) and for entire tuples (@a field == NULL, mp_data points to
 * msgpack array of the tuple)
 * Must return 0 if constraint passed. Must set diag otherwise.
 */
typedef int (*tuple_constraint_f)(const struct tuple_constraint *constraint,
				  const char *mp_data, const char *mp_data_end,
				  const struct tuple_field *field);

/**
 * Type of constraint destructor.
 */
typedef void (*tuple_constraint_destroy_f)(struct tuple_constraint *constraint);

/**
 * Additional data for each local/foreign field pair in foreign key constraint.
 */
struct tuple_constraint_fkey_pair_data {
	/**
	 * Field number of foreign field. Can be -1 if the field was not found
	 * by name.
	 */
	int32_t foreign_field_no;
	/**
	 * Field number of local field. Can be -1 if the field was not found
	 * by name.
	 */
	int32_t local_field_no;
};

/**
 * Additional data for foreign key constraints.
 */
struct tuple_constraint_fkey_data {
	/**
	 * Index number (dense ID) in local space that is unique and build by
	 * field(s) of this constraint. Can be -1 if there's no such index.
	 */
	int32_t local_index;
	/**
	 * Index number (dense ID) in foreign space that is unique and build by
	 * field(s) that this constraint refers to. Can be -1 if there's no
	 * such index.
	 */
	int32_t foreign_index;
	/**
	 * Number of local/foreign field pairs that participate in foreign key.
	 */
	uint32_t field_count;
	/**
	 * Array of data of each local/foreign field pair.
	 */
	struct tuple_constraint_fkey_pair_data data[];
};

/**
 * Generic constraint of a tuple or a tuple field.
 */
struct tuple_constraint {
	/** Constraint definition. */
	struct tuple_constraint_def def;
	/** The constraint check function. */
	tuple_constraint_f check;
	/** Destructor. Expected to be reentrant - it's ok to call it twice.*/
	tuple_constraint_destroy_f destroy;
	/** Space in which the constraint is. */
	struct space *space;
	/** Various data for different states of constraint. */
	union {
		/** Data of pinned function in func cache. */
		struct func_cache_holder func_cache_holder;
		/** Data of pinned foreign space in space cache. */
		struct space_cache_holder space_cache_holder;
		/** Data of subscription in space cache. */
	};
	/** Additional data for foreign key constraints. */
	struct tuple_constraint_fkey_data *fkey;
};

/**
 * Check that checks nothing and always passes. Used as a default.
 */
int
tuple_constraint_noop_check(const struct tuple_constraint *constraint,
			    const char *mp_data, const char *mp_data_end,
			    const struct tuple_field *field);

/**
 * No-op destructor of constraint. Used as a default.
 */
void
tuple_constraint_noop_destructor(struct tuple_constraint *constr);

/**
 * Compare two constraint objects, return 0 if they are equal.
 * Don't compare function pointers, only constraint definition.
 * If @a ignore_name is true then ignore constraint's name and compare only
 * constraint's entity.
 */
int
tuple_constraint_cmp(const struct tuple_constraint *constr1,
		     const struct tuple_constraint *constr2,
		     bool ignore_name);

/**
 * Append tuple constraint to hash calculation using PMurHash32_Process.
 * Process only string names, don't take function pointers into consideration.
 * Return size of processed data.
 */
uint32_t
tuple_constraint_hash_process(const struct tuple_constraint *constr,
			      uint32_t *ph, uint32_t *pcarry);

/**
 * Allocate a single memory block needed for given @a count of constraints by
 * given definitions @a def, including strings in them.
 * Fill the block with strings and construct constraints using new strings.
 * The resulting array is default initialized constraints with deeply copied
 * definitions allocated in one memory block. Needs one free() call in the end.
 * Never fail (uses xmalloc); return NULL if constraint_count == 0.
 *
 * @param def - array of given constraint definitions.
 * @param constraint_count - number of given constraints.
 * @return a single memory block with constraints.
 */
struct tuple_constraint *
tuple_constraint_collocate(const struct tuple_constraint_def *def,
			   size_t constraint_count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#include "box/box.h"
#include "box/coll_id_cache.h"
#include "box/coll_id_def.h"
#include "box/sql.h"
#include "box/tuple.h"
#include "box/tuple_format.h"

#include "coll/coll.h"

#include "core/fiber.h"
#include "core/memory.h"

#include "mpstream/mpstream.h"

#include "msgpuck/msgpuck.h"

#define UNIT_TAP_COMPATIBLE 1
#include "unit.h"

static void
mpstream_error(void *is_err)
{
	*(bool *)is_err = true;
}

/*
 * Checks that tuple format comparison for runtime tuple formats works
 * correctly.
 */
static int
test_tuple_format_cmp(void)
{
	plan(17);
	header();

	char buf[1024];
	size_t size = mp_format(buf, lengthof(buf), "[{%s%s} {%s%s}]",
				"name", "f1", "name", "f2");
	struct tuple_format *f1 = runtime_tuple_format_new(buf, size, false);
	struct tuple_format *f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same field counts are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s}]", "name", "f1");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different field counts are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s}]", "name", "f1");
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same 'name' definitions are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s}]", "name", "f2");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different 'name' definitions "
	   "are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%s}]",
			 "name", "f", "type", "integer");
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same 'name' definitions are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%s}]",
			 "name", "f", "type", "string");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different 'type' definitions "
	   "are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%s}]",
			 "name", "f", "nullable_action", "default");
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same 'is_nullable' definitions "
	   "are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%b %s%s}]",
			 "name", "f", "is_nullable", true,
			 "nullable_action", "none");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different 'is_nullable' definitions "
	   "are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	struct coll_def coll_def;
	memset(&coll_def, 0, sizeof(coll_def));
	snprintf(coll_def.locale, sizeof(coll_def.locale), "%s", "ru_RU");
	coll_def.type = COLL_TYPE_ICU;
	coll_def.icu.strength = COLL_ICU_STRENGTH_IDENTICAL;
	struct coll_id_def coll_id_def = {
		.id = 1,
		.owner_id = 0,
		.name_len = strlen("c1"),
		.name = "c1",
		.base = coll_def,
	};
	struct coll_id *coll_id1 = coll_id_new(&coll_id_def);
	coll_id_def.id = 2;
	coll_id_def.name = "c2";
	struct coll_id *coll_id2 = coll_id_new(&coll_id_def);
	struct coll_id *replaced_id;
	coll_id_cache_replace(coll_id1, &replaced_id);
	coll_id_cache_replace(coll_id2, &replaced_id);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%d}]",
			 "name", "f", "collation", 1);
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same 'collation' definitions "
	   "are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%d}]",
			 "name", "f", "collation", 2);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different 'collation' definitions "
	   "are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	coll_id_cache_delete(coll_id2);
	coll_id_cache_delete(coll_id1);
	coll_id_delete(coll_id2);
	coll_id_delete(coll_id1);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%s}]",
			 "name", "f", "sql_default", "1 + 1");
	f1 = runtime_tuple_format_new(buf, size, false);
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%s}]",
			 "name", "f", "sql_default", "2");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different expressions in "
	   "'sql_default' definitions are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s{%s%d %s%d}}]",
			 "name", "f", "constraint", "c1", 1,
			 "c2", 2);
	f1 = runtime_tuple_format_new(buf, size, false);
	size = mp_format(buf, lengthof(buf), "[{%s%s %s{%s%d}}]",
			 "name", "f", "constraint", "c1", 1);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different number of constraints in "
	   "'constraint' definitions are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s{%s%d}}]",
			 "name", "f", "constraint", "c1", 1);
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same 'constraint' definitions "
	   "are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s %s{%s%d}}]",
			 "name", "f", "constraint", "c2", 2);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different 'constraint' definitions "
	   "are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%p}]",
			 "name", "f", "default", "\xcc\x00");
	f1 = runtime_tuple_format_new(buf, size, false);
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%p}]",
			 "name", "f", "default", "\x01");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different MsgPack sizes of 'default' "
	   "definitions are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	size = mp_format(buf, lengthof(buf), "[{%s%s %s%p}]",
			 "name", "f", "default", "\x00");
	f1 = runtime_tuple_format_new(buf, size, false);
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 == f2, "tuple formats with same MsgPacks 'default' definitions "
	   "are equal");
	size = mp_format(buf, lengthof(buf), "[{%s%s %s%p}]",
			 "name", "f", "default", "\x01");
	f2 = runtime_tuple_format_new(buf, size, false);
	ok(f1 != f2, "tuple formats with different MsgPacks of 'default' "
	   "definitions are not equal");
	tuple_format_delete(f1);
	tuple_format_delete(f2);

	footer();
	return check_plan();
}

static int
test_tuple_format_to_mpstream(void)
{
	plan(1);
	header();

	struct mpstream stream;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	bool is_err = false;
	mpstream_init(&stream, region, region_reserve_cb, region_alloc_cb,
		      mpstream_error, &is_err);
	tuple_format_to_mpstream(tuple_format_runtime, &stream);
	mpstream_flush(&stream);
	fail_if(is_err);
	size_t data_len = region_used(region) - region_svp;
	const char *data = xregion_join(region, data_len);
	char buf[1024];
	char *w = mp_encode_uint(buf, tuple_format_runtime->id);
	w = mp_encode_array(w, 0);
	size_t expected_len = w - buf;
	is(memcmp(data, buf, MIN(data_len, expected_len)), 0,
	   "tuple format serialization works correctly");
	region_truncate(region, region_svp);

	footer();
	return check_plan();
}

static int
test_tuple_format(void)
{
	plan(2);
	header();

	test_tuple_format_cmp();
	test_tuple_format_to_mpstream();

	footer();
	return check_plan();
}

static uint32_t
test_field_name_hash(const char *str, uint32_t len)
{
	return str[0] + len;
}

int
main(void)
{
	memory_init();
	fiber_init(fiber_c_invoke);
	coll_init();
	tuple_init(test_field_name_hash);
	box_init();
	sql_init();

	int rc = test_tuple_format();

	box_free();
	tuple_free();
	coll_free();
	fiber_free();
	memory_free();
	return rc;
}

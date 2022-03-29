#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "unit.h"
#include "core/prbuf.h"
#include "trivia/util.h"

const size_t buffer_size_arr[] = { 128, 256, 512 };
const size_t copy_number_arr[] = { 16, 32, 64 };
const char payload_small[] = { 0xab, 0xdb, 0xee, 0xcc };
const char payload_avg[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };
#define PAYLOAD_LARGE_SIZE 40
static char payload_large[PAYLOAD_LARGE_SIZE];

enum test_buffer_status {
	OK = 0,
	WRONG_PAYLOAD_SIZE,
	WRONG_PAYLOAD_CONTENT,
	RECOVERY_ERROR,
	ALLOCATION_ERROR,
	TEST_BUFFER_STATUS_COUNT
};

const char *info_msg = "prbuf(size=%lu, payload=%lu, iterations=%lu) %s";

const char *test_buffer_status_strs[TEST_BUFFER_STATUS_COUNT] = {
	"has been validated",
	"failed due to wrong size of payload after recovery",
	"failed due to wrong content of payload after recovery",
	"failed to recover",
	"failed to allocate memory"
};

static void
payload_large_init()
{
	for (size_t i = 0; i < PAYLOAD_LARGE_SIZE; ++i)
		payload_large[i] = (char) i;
}

static int
test_buffer(uint32_t buffer_size, const char *payload, uint32_t payload_size,
	    size_t copy_number)
{
	int rc = OK;
	char *mem = (char *) malloc(buffer_size);
	assert(mem != NULL);

	struct prbuf buf;
	prbuf_create(&buf, mem, buffer_size);

	for (size_t i = 0; i < copy_number; ++i) {
		char *p = prbuf_prepare(&buf, payload_size);
		if (p == NULL) {
			rc = ALLOCATION_ERROR;
			goto finish;
		}
		memcpy(p, payload, payload_size);
		prbuf_commit(&buf);
	}

	struct prbuf recovered_buf;
	if (prbuf_open(&recovered_buf, mem) != 0) {
		rc = RECOVERY_ERROR;
		goto finish;
	}

	struct prbuf_iterator iter;
	struct prbuf_entry entry;
	prbuf_iterator_create(&recovered_buf, &iter);
	while (prbuf_iterator_next(&iter, &entry) == 0 &&
	       ! prbuf_entry_is_invalid(&entry)) {
		if (entry.size != payload_size) {
			rc = WRONG_PAYLOAD_SIZE;
			goto finish;
		}
		if (memcmp(payload, entry.ptr, payload_size) != 0) {
			rc = WRONG_PAYLOAD_CONTENT;
			goto finish;
		}
	};

finish:
	free(mem);
	return rc;
}

static void
test_buffer_foreach_copy_number(uint32_t buffer_size,
				const char *payload, uint32_t payload_size)
{
	header();
	plan(lengthof(copy_number_arr));
	int rc = 0;
	for (size_t i = 0; i < lengthof(copy_number_arr); ++i) {
		rc = test_buffer(buffer_size, payload, payload_size,
				 copy_number_arr[i]);
		is(rc, 0, info_msg, buffer_size, payload_size,
		   copy_number_arr[i], test_buffer_status_strs[rc]);
	}
	rc = check_plan();
	footer();

}

static void
test_buffer_foreach_payload(uint32_t buffer_size)
{
	test_buffer_foreach_copy_number(buffer_size, payload_small,
					lengthof(payload_small));
	test_buffer_foreach_copy_number(buffer_size, payload_avg,
					lengthof(payload_avg));
	test_buffer_foreach_copy_number(buffer_size, payload_large,
					PAYLOAD_LARGE_SIZE);
}

static void
test_buffer_foreach_size()
{
	for (size_t i = 0; i < lengthof(buffer_size_arr); ++i)
		test_buffer_foreach_payload(buffer_size_arr[i]);
}

/**
 * There are three possible configurations of test:
 * 1. The size of buffer;
 * 2. The size of payload;
 * 3. The number of saves to the buffer.
 */
int
main(void)
{
	payload_large_init();
	test_buffer_foreach_size();
}

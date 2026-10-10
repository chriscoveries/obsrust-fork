#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#include "librtmp/amf.h"

/* librtmp's AMF decoder on nested objects and arrays, as an RTMP server
 * sends them in command and metadata messages. Each nesting level is a
 * recursion, so the decoder fails past AMF_MAX_DEPTH (128) levels instead of
 * running out of stack on a deeply nested message. */

#define MAX_DEPTH 128
#define DEEP 200000

struct buf {
	char *data;
	size_t len;
	size_t cap;
};

static void put(struct buf *b, const char *bytes, size_t len)
{
	if (b->len + len > b->cap) {
		b->cap = (b->cap + len) * 2;
		b->data = realloc(b->data, b->cap);
		assert_non_null(b->data);
	}
	memcpy(b->data + b->len, bytes, len);
	b->len += len;
}

#define PUT(b, ...)                                           \
	do {                                                  \
		const char bytes_[] = {__VA_ARGS__};          \
		put((b), bytes_, sizeof(bytes_));             \
	} while (0)

/* AMF0: `depth` objects, each holding the next as property "a" */
static struct buf nested_objects(size_t depth)
{
	struct buf b = {0};
	PUT(&b, AMF_OBJECT);
	for (size_t i = 1; i < depth; i++)
		PUT(&b, 0, 1, 'a', AMF_OBJECT);
	for (size_t i = 0; i < depth; i++)
		PUT(&b, 0, 0, AMF_OBJECT_END);
	return b;
}

/* AMF0: `depth` strict arrays of one element, the innermost holding a
 * boolean */
static struct buf nested_arrays(size_t depth)
{
	struct buf b = {0};
	for (size_t i = 0; i < depth; i++)
		PUT(&b, AMF_STRICT_ARRAY, 0, 0, 0, 1);
	PUT(&b, AMF_BOOLEAN, 1);
	return b;
}

/* AMF0 ECMA arrays (count, then named properties up to an end marker) */
static struct buf nested_ecma_arrays(size_t depth)
{
	struct buf b = {0};
	PUT(&b, AMF_ECMA_ARRAY, 0, 0, 0, 1);
	for (size_t i = 1; i < depth; i++)
		PUT(&b, 0, 1, 'a', AMF_ECMA_ARRAY, 0, 0, 0, 1);
	for (size_t i = 0; i < depth; i++)
		PUT(&b, 0, 0, AMF_OBJECT_END);
	return b;
}

/* AMF3 inside AMF0: dynamic objects, each holding the next as "a". The
 * nested object's marker is read twice, as type and as object header. */
static struct buf nested_amf3_objects(size_t depth)
{
	struct buf b = {0};
	PUT(&b, AMF_AVMPLUS);
	for (size_t i = 0; i < depth; i++)
		/* object, inline dynamic traits, empty class name, "a" */
		PUT(&b, AMF3_OBJECT, 0x0b, 0x01, 0x03, 'a', AMF3_OBJECT);
	return b;
}

static int decode(struct buf *b)
{
	AMFObject obj;
	int ret = AMF_Decode(&obj, b->data, (int)b->len, FALSE);
	AMF_Reset(&obj);
	free(b->data);
	b->data = NULL;
	return ret;
}

static void test_nested_object(void **state)
{
	(void)state;

	struct buf b = nested_objects(3);
	AMFObject obj;
	assert_int_equal(AMF_Decode(&obj, b.data, (int)b.len, FALSE), (int)b.len);

	/* value -> "a" -> "a" */
	assert_int_equal(AMF_CountProp(&obj), 1);
	AMFObjectProperty *prop = AMF_GetProp(&obj, NULL, 0);
	assert_int_equal(AMFProp_GetType(prop), AMF_OBJECT);
	AVal a = AVC("a");
	AMFObject level;
	for (int i = 0; i < 2; i++) {
		AMFProp_GetObject(prop, &level);
		prop = AMF_GetProp(&level, &a, -1);
		assert_int_equal(AMFProp_GetType(prop), AMF_OBJECT);
	}
	AMFProp_GetObject(prop, &level);
	assert_int_equal(AMF_CountProp(&level), 0);

	AMF_Reset(&obj);
	free(b.data);
}

static void test_depth_limit(void **state)
{
	(void)state;

	struct buf b;
	size_t len;

	b = nested_objects(MAX_DEPTH);
	len = b.len;
	assert_int_equal(decode(&b), (int)len);
	b = nested_objects(MAX_DEPTH + 1);
	assert_int_equal(decode(&b), -1);

	b = nested_arrays(MAX_DEPTH);
	len = b.len;
	assert_int_equal(decode(&b), (int)len);
	b = nested_arrays(MAX_DEPTH + 1);
	assert_int_equal(decode(&b), -1);

	b = nested_ecma_arrays(MAX_DEPTH);
	len = b.len;
	assert_int_equal(decode(&b), (int)len);
	b = nested_ecma_arrays(MAX_DEPTH + 1);
	assert_int_equal(decode(&b), -1);

	b = nested_amf3_objects(MAX_DEPTH + 1);
	assert_int_equal(decode(&b), -1);
}

/* Deep enough to overflow any thread's stack without the limit. */
static void test_deep_nesting(void **state)
{
	(void)state;

	struct buf b;

	b = nested_objects(DEEP);
	assert_int_equal(decode(&b), -1);
	b = nested_arrays(DEEP);
	assert_int_equal(decode(&b), -1);
	b = nested_ecma_arrays(DEEP);
	assert_int_equal(decode(&b), -1);
	b = nested_amf3_objects(DEEP);
	assert_int_equal(decode(&b), -1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_nested_object),
		cmocka_unit_test(test_depth_limit),
		cmocka_unit_test(test_deep_nesting),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}

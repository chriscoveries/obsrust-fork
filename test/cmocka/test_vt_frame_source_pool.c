#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#define OBS_VT_ENCODER_TEST
#include "../../plugins/mac-videotoolbox/encoder.c"

static void test_completed_frame_source_is_reused(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder enc = {0};
	assert_int_equal(pthread_mutex_init(&enc.texture_mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&enc.texture_cond, NULL), 0);
	enc.texture_sync_initialized = true;
	assert_true(vt_init_frame_source_pool(&enc));

	struct vt_frame_source_ref initial = {
		.encoder = &enc,
		.type = VT_FRAME_SOURCE_TEXTURE,
		.references = 1,
	};
	struct vt_frame_source_ref *first = vt_track_new_frame_source(&enc, &initial);
	assert_non_null(first);
	assert_int_equal(enc.pending_frame_submissions, 1);

	struct vt_frame_source_ref *completed = NULL;
	assert_true(vt_take_frame_source(&enc, first, &completed));
	vt_release_texture_source_resources(completed);
	vt_finish_texture_source(completed);
	assert_int_equal(enc.pending_frame_submissions, 0);

	struct vt_frame_source_ref *second = vt_track_new_frame_source(&enc, &initial);
	assert_ptr_equal(second, first);
	assert_true(vt_take_frame_source(&enc, second, &completed));
	vt_release_texture_source_resources(completed);
	vt_finish_texture_source(completed);

	vt_free_frame_source_pool(&enc);
	pthread_cond_destroy(&enc.texture_cond);
	pthread_mutex_destroy(&enc.texture_mutex);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_completed_frame_source_is_reused),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

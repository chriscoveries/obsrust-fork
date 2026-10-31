#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#define OBS_VT_ENCODER_TEST
#include "../../plugins/mac-videotoolbox/encoder.c"

const char *obs_module_text(const char *lookup)
{
	return lookup;
}

struct drain_test_state {
	unsigned complete_calls;
	unsigned invalidate_calls;
	unsigned lease_release_calls;
	unsigned bitrate_set_calls;
	uint32_t restart_bitrate;
};

static struct drain_test_state *active_drain_test;

static OSStatus fail_complete_frames(VTCompressionSessionRef session, CMTime until_presentation_time)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(until_presentation_time);
	active_drain_test->complete_calls++;
	return -1;
}

static void record_session_invalidation(VTCompressionSessionRef session)
{
	UNUSED_PARAMETER(session);
	active_drain_test->invalidate_calls++;
}

static void update_bitrate_settings(struct vt_encoder *enc, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	enc->bitrate = 6000;
}

static OSStatus record_bitrate_update(VTCompressionSessionRef session, uint32_t bitrate)
{
	assert_non_null(session);
	active_drain_test->bitrate_set_calls++;
	active_drain_test->restart_bitrate = bitrate;
	return noErr;
}

static OSStatus recreate_session_with_current_settings(struct vt_encoder *enc)
{
	active_drain_test->restart_bitrate = enc->bitrate;
	enc->session = (VTCompressionSessionRef)(uintptr_t)2;
	return noErr;
}

static void count_lease_release(void *token)
{
	struct drain_test_state *state = token;
	state->lease_release_calls++;
	assert_int_equal(state->invalidate_calls, 1);
}

static void init_test_encoder(struct vt_encoder *enc)
{
	memset(enc, 0, sizeof(*enc));
	assert_int_equal(pthread_mutex_init(&enc->texture_mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&enc->texture_cond, NULL), 0);
	enc->texture_sync_initialized = true;
}

static void destroy_test_encoder(struct vt_encoder *enc)
{
	vt_clear_frame_source_tombstones(enc);
	pthread_cond_destroy(&enc->texture_cond);
	pthread_mutex_destroy(&enc->texture_mutex);
}

static void test_complete_frames_failure_invalidates_and_releases_once(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder enc;
	struct drain_test_state drain_state = {0};
	struct vt_frame_source_ref *source_ref = bzalloc(sizeof(*source_ref));
	assert_non_null(source_ref);

	init_test_encoder(&enc);
	active_drain_test = &drain_state;
	enc.session = (VTCompressionSessionRef)(uintptr_t)1;
	enc.complete_frames_for_test = fail_complete_frames;
	enc.invalidate_session_for_test = record_session_invalidation;

	source_ref->encoder = &enc;
	source_ref->lease_token = &drain_state;
	source_ref->lease_release = count_lease_release;
	source_ref->references = 1;
	enc.frame_sources = source_ref;
	enc.pending_frame_submissions = 1;

	vt_drain_texture_encode(&enc);

	assert_int_equal(drain_state.complete_calls, 1);
	assert_int_equal(drain_state.invalidate_calls, 1);
	assert_int_equal(drain_state.lease_release_calls, 1);
	assert_int_equal(enc.pending_frame_submissions, 0);
	assert_null(enc.session);

	/* A callback already in flight after invalidation finds the tombstone and
	 * cannot release its lease a second time. */
	struct vt_frame_source_ref *callback_ref = NULL;
	assert_true(vt_take_frame_source(&enc, source_ref, &callback_ref));
	vt_release_texture_source_resources(callback_ref);
	vt_finish_texture_source(callback_ref);
	assert_int_equal(drain_state.lease_release_calls, 1);
	assert_int_equal(enc.pending_frame_submissions, 0);
	assert_null(enc.frame_sources);

	destroy_test_encoder(&enc);
	active_drain_test = NULL;
}

static void test_late_callback_after_failed_drain_and_destroy_is_ignored(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder *enc = bzalloc(sizeof(*enc));
	struct drain_test_state drain_state = {0};
	struct vt_frame_source_ref *source_ref = bzalloc(sizeof(*source_ref));
	assert_non_null(enc);
	assert_non_null(source_ref);

	init_test_encoder(enc);
	active_drain_test = &drain_state;
	enc->session = (VTCompressionSessionRef)(uintptr_t)1;
	enc->complete_frames_for_test = fail_complete_frames;
	enc->invalidate_session_for_test = record_session_invalidation;
	void *callback_refcon = vt_register_callback_context(enc);
	assert_non_null(callback_refcon);

	source_ref->encoder = enc;
	source_ref->lease_token = &drain_state;
	source_ref->lease_release = count_lease_release;
	source_ref->type = VT_FRAME_SOURCE_TEXTURE;
	source_ref->references = 1;
	enc->frame_sources = source_ref;
	enc->pending_frame_submissions = 1;

	/* Drain invalidates the session before releasing the record. Destroy then
	 * removes the callback context and frees the tombstone. */
	vt_drain_texture_encode(enc);
	assert_int_equal(drain_state.invalidate_calls, 1);
	assert_int_equal(drain_state.lease_release_calls, 1);
	assert_null(vt_acquire_callback_context(callback_refcon));
	vt_destroy(enc);

	/* VideoToolbox may still deliver this callback with both refcons stale. The
	 * callback must reject the unknown session ID before dereferencing either. */
	sample_encoded_callback(callback_refcon, source_ref, noErr, 0, NULL);
	assert_int_equal(drain_state.lease_release_calls, 1);
	active_drain_test = NULL;
}

static void test_bitrate_update_after_failed_drain_is_deferred_until_restart(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder *enc = bzalloc(sizeof(*enc));
	struct drain_test_state drain_state = {0};
	assert_non_null(enc);

	init_test_encoder(enc);
	active_drain_test = &drain_state;
	enc->session = (VTCompressionSessionRef)(uintptr_t)1;
	enc->complete_frames_for_test = fail_complete_frames;
	enc->invalidate_session_for_test = record_session_invalidation;
	assert_non_null(vt_register_callback_context(enc));
	vt_drain_texture_encode(enc);
	assert_null(enc->session);

	enc->bitrate = 4000;
	enc->update_params_for_test = update_bitrate_settings;
	enc->set_bitrate_for_test = record_bitrate_update;
	enc->create_session_for_test = recreate_session_with_current_settings;
	assert_true(vt_update(enc, NULL));
	assert_int_equal(enc->bitrate, 6000);
	assert_int_equal(drain_state.bitrate_set_calls, 0);

	assert_true(vt_ensure_session(enc));
	assert_int_equal(drain_state.restart_bitrate, 6000);
	assert_false(enc->session_invalidated);
	enc->session = NULL;
	vt_destroy(enc);
	active_drain_test = NULL;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_complete_frames_failure_invalidates_and_releases_once),
		cmocka_unit_test(test_late_callback_after_failed_drain_and_destroy_is_ignored),
		cmocka_unit_test(test_bitrate_update_after_failed_drain_is_deferred_until_restart),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

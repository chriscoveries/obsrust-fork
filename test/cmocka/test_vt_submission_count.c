/* SPDX-License-Identifier: GPL-2.0-or-later */
/* vt_encode() must take one measurement slot per frame and commit it once
 * VideoToolbox accepts the frame. The zero-copy harness stops at a slot
 * limit, so a frame that takes two slots ends a raw run at half the limit.
 * The real encoder runs with its pool lookup and its submission replaced. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>
#include <VideoToolbox/VideoToolbox.h>

#define TEST_WIDTH 64
#define TEST_HEIGHT 32
#define TEST_FRAMES 3

static CVPixelBufferPoolRef test_pool;

static CVPixelBufferPoolRef test_session_pool(VTCompressionSessionRef session)
{
	(void)session;
	return test_pool;
}

#define OBS_VT_ENCODER_TEST
#define VTCompressionSessionGetPixelBufferPool test_session_pool
#include "../../plugins/mac-videotoolbox/encoder.c"
#undef VTCompressionSessionGetPixelBufferPool

static unsigned submitted_frames;

static OSStatus accept_test_frame(struct vt_encoder *enc, CVPixelBufferRef pixbuf, CMTime pts, CMTime duration,
				  void *source_refcon)
{
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(duration);
	assert_non_null(pixbuf);
	submitted_frames++;
	void *callback_refcon = (void *)(uintptr_t)enc->callback_context->id;
	sample_encoded_callback(callback_refcon, source_refcon, noErr, 0, NULL);
	return noErr;
}

static CVPixelBufferPoolRef create_test_pool(OSType pixel_format)
{
	int width = TEST_WIDTH;
	int height = TEST_HEIGHT;
	int format = (int)pixel_format;
	CFNumberRef width_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &width);
	CFNumberRef height_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &height);
	CFNumberRef format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &format);
	const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey};
	const void *values[] = {width_number, height_number, format_number};
	CFDictionaryRef attributes = CFDictionaryCreate(
		kCFAllocatorDefault, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(width_number);
	CFRelease(height_number);
	CFRelease(format_number);

	CVPixelBufferPoolRef pool = NULL;
	assert_int_equal(CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, attributes, &pool), kCVReturnSuccess);
	CFRelease(attributes);
	assert_non_null(pool);
	return pool;
}

static void test_raw_encode_takes_one_slot_per_accepted_frame(void **state)
{
	UNUSED_PARAMETER(state);
	const OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	struct vt_encoder enc;
	memset(&enc, 0, sizeof(enc));
	assert_int_equal(pthread_mutex_init(&enc.texture_mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&enc.texture_cond, NULL), 0);
	enc.texture_sync_initialized = true;
	enc.width = TEST_WIDTH;
	enc.height = TEST_HEIGHT;
	enc.fps_num = 30;
	enc.fps_den = 1;
	enc.vt_pix_fmt = pixel_format;
	enc.colorspace = VIDEO_CS_709;
	enc.session = (VTCompressionSessionRef)(uintptr_t)1;
	enc.encode_frame_for_test = accept_test_frame;
	assert_int_equal(CMSimpleQueueCreate(kCFAllocatorDefault, 4, &enc.queue), noErr);
	assert_non_null(vt_register_callback_context(&enc));
	test_pool = create_test_pool(pixel_format);
	submitted_frames = 0;

	static uint8_t luma[TEST_WIDTH * TEST_HEIGHT];
	static uint8_t chroma[TEST_WIDTH * TEST_HEIGHT / 2];
	struct encoder_frame frame = {
		.data = {luma, chroma},
		.linesize = {TEST_WIDTH, TEST_WIDTH},
	};

	vt_copy_bytes_reset();
	vt_encode_submission_reset(TEST_FRAMES);
	for (int i = 0; i < TEST_FRAMES; i++) {
		struct encoder_packet packet = {0};
		bool received_packet = false;
		frame.pts = i;
		assert_true(vt_encode(&enc, &frame, &packet, &received_packet));
		assert_false(received_packet);
	}

	assert_int_equal(submitted_frames, TEST_FRAMES);
	assert_int_equal(vt_encode_submission_total(), TEST_FRAMES);
	assert_int_equal(vt_copy_bytes_total(), (uint64_t)TEST_FRAMES * TEST_WIDTH * TEST_HEIGHT * 3 / 2);
	assert_int_equal(enc.pending_frame_submissions, 0);
	assert_null(enc.frame_sources);

	vt_unregister_callback_context(&enc);
	CFRelease(enc.queue);
	CVPixelBufferPoolRelease(test_pool);
	test_pool = NULL;
	vt_clear_frame_source_tombstones(&enc);
	pthread_cond_destroy(&enc.texture_cond);
	pthread_mutex_destroy(&enc.texture_mutex);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_raw_encode_takes_one_slot_per_accepted_frame),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

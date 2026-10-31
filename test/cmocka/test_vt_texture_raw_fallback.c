#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#define OBS_VT_ENCODER_TEST
#include "../../plugins/mac-videotoolbox/encoder.c"

struct raw_fallback_test_state {
	IOSurfaceRef source_surface;
	unsigned wait_calls;
	unsigned wrap_calls;
	unsigned cached_buffer_calls;
	unsigned raw_submission_calls;
	size_t bytes_compared;
};

static struct raw_fallback_test_state *active_raw_fallback_test;

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

static CVPixelBufferRef create_test_surface_pixel_buffer(uint32_t width, uint32_t height, OSType pixel_format)
{
	CFDictionaryRef surface_properties = CFDictionaryCreate(
		kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
	const void *values[] = {surface_properties};
	CFDictionaryRef attributes = CFDictionaryCreate(
		kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(surface_properties);

	CVPixelBufferRef pixel_buffer = NULL;
	assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixel_format, attributes,
					     &pixel_buffer),
			 kCVReturnSuccess);
	CFRelease(attributes);
	assert_non_null(pixel_buffer);
	return pixel_buffer;
}

static void fill_test_surface(CVPixelBufferRef pixel_buffer)
{
	assert_int_equal(CVPixelBufferLockBaseAddress(pixel_buffer, 0), kCVReturnSuccess);
	assert_int_equal(CVPixelBufferGetPlaneCount(pixel_buffer), 2);
	for (size_t plane = 0; plane < 2; plane++) {
		uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane);
		size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane);
		size_t height = CVPixelBufferGetHeightOfPlane(pixel_buffer, plane);
		assert_non_null(base);
		for (size_t row = 0; row < height; row++)
			for (size_t col = 0; col < stride; col++)
				base[row * stride + col] = (uint8_t)(plane * 71 + row * 13 + col * 7);
	}
	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixel_buffer, 0), kCVReturnSuccess);
}

static bool wait_for_texture_completion(gs_texture_t *texture)
{
	assert_non_null(texture);
	active_raw_fallback_test->wait_calls++;
	return true;
}

static CVReturn fail_texture_surface_wrap(IOSurfaceRef surface, CVPixelBufferRef *pixbuf)
{
	assert_non_null(surface);
	active_raw_fallback_test->wrap_calls++;
	*pixbuf = NULL;
	return kCVReturnError;
}

static bool create_cached_test_pixel_buffer(struct vt_encoder *enc, CVPixelBufferRef *pixbuf)
{
	active_raw_fallback_test->cached_buffer_calls++;
	return CVPixelBufferCreate(kCFAllocatorDefault, enc->width, enc->height, enc->vt_pix_fmt, NULL, pixbuf) ==
	       kCVReturnSuccess;
}

static void *unused_lease_acquire(void *context)
{
	return context;
}

static void unused_lease_release(void *token)
{
	UNUSED_PARAMETER(token);
}

static OSStatus submit_raw_test_frame(struct vt_encoder *enc, CVPixelBufferRef pixbuf, CMTime pts, CMTime duration,
				      void *source_refcon)
{
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(duration);
	struct raw_fallback_test_state *test = active_raw_fallback_test;
	test->raw_submission_calls++;
	assert_non_null(pixbuf);
	assert_non_null(source_refcon);
	assert_int_equal(CVPixelBufferLockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
	assert_int_equal(IOSurfaceLock(test->source_surface, kIOSurfaceLockReadOnly, NULL), kIOReturnSuccess);

	for (size_t plane = 0; plane < 2; plane++) {
		uint8_t *actual = CVPixelBufferGetBaseAddressOfPlane(pixbuf, plane);
		uint8_t *expected = IOSurfaceGetBaseAddressOfPlane(test->source_surface, plane);
		size_t actual_stride = CVPixelBufferGetBytesPerRowOfPlane(pixbuf, plane);
		size_t expected_stride = IOSurfaceGetBytesPerRowOfPlane(test->source_surface, plane);
		size_t height = IOSurfaceGetHeightOfPlane(test->source_surface, plane);
		assert_non_null(actual);
		assert_non_null(expected);
		assert_true(actual_stride >= enc->width);
		assert_true(expected_stride >= enc->width);
		for (size_t row = 0; row < height; row++) {
			assert_memory_equal(actual + row * actual_stride, expected + row * expected_stride, enc->width);
			test->bytes_compared += enc->width;
		}
	}
	assert_int_equal(IOSurfaceUnlock(test->source_surface, kIOSurfaceLockReadOnly, NULL), kIOReturnSuccess);
	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixbuf, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);

	void *callback_refcon = (void *)(uintptr_t)enc->callback_context->id;
	sample_encoded_callback(callback_refcon, source_refcon, noErr, 0, NULL);
	return noErr;
}

static void test_texture_fallback_calls_raw_fallback_validation(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder enc;
	init_test_encoder(&enc);
	enc.width = 1920;
	enc.height = 1080;
	enc.vt_pix_fmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	enc.session = (VTCompressionSessionRef)(uintptr_t)1;

	CVPixelBufferRef wrong_size = NULL;
	assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, 640, 480, enc.vt_pix_fmt, NULL, &wrong_size),
			 kCVReturnSuccess);
	assert_non_null(wrong_size);

	struct encoder_packet packet = {0};
	bool received_packet = false;
	assert_false(vt_encode_texture_raw_fallback(&enc, wrong_size, NULL, 0, &packet, &received_packet));
	assert_true(enc.texture_fallback_logged);
	assert_false(received_packet);

	CFRelease(wrong_size);
	destroy_test_encoder(&enc);
}

static void test_encode_texture_uses_counted_raw_fallback_after_surface_wrap_fails(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder enc;
	struct raw_fallback_test_state test = {0};
	const OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	init_test_encoder(&enc);
	active_raw_fallback_test = &test;
	enc.width = 64;
	enc.height = 32;
	enc.fps_num = 30;
	enc.fps_den = 1;
	enc.vt_pix_fmt = pixel_format;
	enc.session = (VTCompressionSessionRef)(uintptr_t)1;
	enc.wait_for_completion_for_test = wait_for_texture_completion;
	enc.create_pixbuf_with_surface_for_test = fail_texture_surface_wrap;
	enc.get_cached_pixel_buffer_for_test = create_cached_test_pixel_buffer;
	enc.encode_frame_for_test = submit_raw_test_frame;
	assert_int_equal(CMSimpleQueueCreate(kCFAllocatorDefault, 4, &enc.queue), noErr);
	assert_non_null(vt_register_callback_context(&enc));

	CVPixelBufferRef source_pixel_buffer = create_test_surface_pixel_buffer(enc.width, enc.height, pixel_format);
	fill_test_surface(source_pixel_buffer);
	test.source_surface = CVPixelBufferGetIOSurface(source_pixel_buffer);
	assert_non_null(test.source_surface);
	CFRetain(test.source_surface);

	struct encoder_texture texture = {
		.tex = {(gs_texture_t *)(uintptr_t)1, (gs_texture_t *)(uintptr_t)2},
		.iosurface = test.source_surface,
		.lease_acquire = unused_lease_acquire,
		.lease_release = unused_lease_release,
	};
	struct encoder_packet packet = {0};
	bool received_packet = false;
	assert_true(vt_encode_texture(&enc, &texture, 7, 0, NULL, &packet, &received_packet));
	assert_false(received_packet);
	assert_true(enc.texture_fallback_logged);
	assert_int_equal(test.wait_calls, 1);
	assert_int_equal(test.wrap_calls, 1);
	assert_int_equal(test.cached_buffer_calls, 1);
	assert_int_equal(test.raw_submission_calls, 1);
	assert_int_equal(test.bytes_compared, enc.width * enc.height * 3 / 2);
	assert_int_equal(enc.pending_frame_submissions, 0);
	assert_null(enc.frame_sources);

	vt_unregister_callback_context(&enc);
	CFRelease(enc.queue);
	CFRelease(test.source_surface);
	CFRelease(source_pixel_buffer);
	destroy_test_encoder(&enc);
	active_raw_fallback_test = NULL;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_texture_fallback_calls_raw_fallback_validation),
		cmocka_unit_test(test_encode_texture_uses_counted_raw_fallback_after_surface_wrap_fails),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

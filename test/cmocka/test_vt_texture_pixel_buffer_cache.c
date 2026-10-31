#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#define OBS_VT_ENCODER_TEST
#include "../../plugins/mac-videotoolbox/encoder.c"

static unsigned pixel_buffer_creations;

static CVReturn create_counted_pixel_buffer(IOSurfaceRef surface, CVPixelBufferRef *pixel_buffer)
{
	pixel_buffer_creations++;
	return CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, NULL, pixel_buffer);
}

static CVPixelBufferRef create_surface_pixel_buffer(void)
{
	CFDictionaryRef surface_properties = CFDictionaryCreate(
		kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
	const void *values[] = {surface_properties};
	CFDictionaryRef attributes = CFDictionaryCreate(
		kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(surface_properties);

	CVPixelBufferRef pixel_buffer = NULL;
	assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, 64, 32,
					     kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes,
					     &pixel_buffer),
			 kCVReturnSuccess);
	CFRelease(attributes);
	assert_non_null(pixel_buffer);
	assert_non_null(CVPixelBufferGetIOSurface(pixel_buffer));
	return pixel_buffer;
}

static void test_texture_pixel_buffer_is_created_once_per_surface(void **state)
{
	UNUSED_PARAMETER(state);
	struct vt_encoder enc = {0};
	enc.create_pixbuf_with_surface_for_test = create_counted_pixel_buffer;
	pixel_buffer_creations = 0;

	CVPixelBufferRef surface_buffer = create_surface_pixel_buffer();
	IOSurfaceRef surface = CVPixelBufferGetIOSurface(surface_buffer);
	CVPixelBufferRef first = NULL;
	CVPixelBufferRef second = NULL;
	assert_true(vt_get_texture_pixel_buffer(&enc, surface, &first));
	assert_true(vt_get_texture_pixel_buffer(&enc, surface, &second));
	assert_non_null(first);
	assert_ptr_equal(first, second);
	assert_int_equal(pixel_buffer_creations, 1);

	CFRelease(first);
	CFRelease(second);
	vt_clear_texture_pixel_buffer_cache(&enc);
	CFRelease(surface_buffer);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_texture_pixel_buffer_is_created_once_per_surface),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

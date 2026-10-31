/* Bytes-copied baseline for the Mac zero-copy epic (fork #47).
 *
 * This is the committed fixture for the copy→M2→M1 ladder: a 1080p NV12
 * frame copies exactly Y(1920x1080) + UV(1920x540) bytes through the
 * encoder upload path, plus one GPU-sync wait per download_frame map
 * (counted separately once the staging seam lands).
 *
 * macOS only: it drives a real CVPixelBuffer, no OBS session needed.
 * M1's zero-copy path must drive these counts to 0 for the same input.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include <CoreVideo/CoreVideo.h>

#include <util/c99defs.h>

#include "vt-frame-copy.h"

#define WIDTH 1920
#define HEIGHT 1080

/* Y + UV with tight linesizes. */
#define EXPECTED_BYTES ((size_t)WIDTH * HEIGHT + (size_t)WIDTH * HEIGHT / 2)

static CVPixelBufferRef make_nv12_buffer(void)
{
	CVPixelBufferRef pixbuf = NULL;
	CVReturn code = CVPixelBufferCreate(NULL, WIDTH, HEIGHT, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL,
					    &pixbuf);
	assert_int_equal(code, kCVReturnSuccess);
	assert_non_null(pixbuf);
	return pixbuf;
}

static void fill_frame(uint8_t *data[MAX_AV_PLANES], uint32_t linesize[MAX_AV_PLANES])
{
	static uint8_t y[WIDTH * HEIGHT];
	static uint8_t uv[WIDTH * HEIGHT / 2];
	for (size_t i = 0; i < sizeof(y); i++)
		y[i] = (uint8_t)(i & 0xFF);
	for (size_t i = 0; i < sizeof(uv); i++)
		uv[i] = (uint8_t)((i * 3) & 0xFF);

	data[0] = y;
	data[1] = uv;
	data[2] = NULL;
	linesize[0] = WIDTH;
	linesize[1] = WIDTH;
	linesize[2] = 0;
}

static void test_copies_nv12_1080p(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t *data[MAX_AV_PLANES] = {0};
	uint32_t linesize[MAX_AV_PLANES] = {0};
	fill_frame(data, linesize);

	CVPixelBufferRef pixbuf = make_nv12_buffer();
	assert_int_equal(CVPixelBufferLockBaseAddress(pixbuf, 0), kCVReturnSuccess);

	vt_copy_bytes_reset();
	size_t copied = vt_copy_frame_to_pixbuf(pixbuf, data, linesize);

	assert_int_equal(copied, EXPECTED_BYTES);
	assert_int_equal(vt_copy_bytes_total(), EXPECTED_BYTES);

	/* Spot-check the bytes actually landed per plane. */
	uint8_t *py = CVPixelBufferGetBaseAddressOfPlane(pixbuf, 0);
	uint8_t *puv = CVPixelBufferGetBaseAddressOfPlane(pixbuf, 1);
	assert_non_null(py);
	assert_non_null(puv);
	assert_int_equal(py[0], 0);
	assert_int_equal(py[WIDTH * HEIGHT - 1], (uint8_t)((WIDTH * HEIGHT - 1) & 0xFF));
	assert_int_equal(puv[0], 0);

	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	CVPixelBufferRelease(pixbuf);
}

static void test_stops_at_null_plane(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t *data[MAX_AV_PLANES] = {0};
	uint32_t linesize[MAX_AV_PLANES] = {0};
	fill_frame(data, linesize);
	data[1] = NULL;

	CVPixelBufferRef pixbuf = make_nv12_buffer();
	assert_int_equal(CVPixelBufferLockBaseAddress(pixbuf, 0), kCVReturnSuccess);

	vt_copy_bytes_reset();
	size_t copied = vt_copy_frame_to_pixbuf(pixbuf, data, linesize);

	assert_int_equal(copied, (size_t)WIDTH * HEIGHT);
	assert_int_equal(vt_copy_bytes_total(), (size_t)WIDTH * HEIGHT);

	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	CVPixelBufferRelease(pixbuf);
}

static void test_submission_copy_bytes_commit_only_after_acceptance(void **state)
{
	UNUSED_PARAMETER(state);
	uint8_t *data[MAX_AV_PLANES] = {0};
	uint32_t linesize[MAX_AV_PLANES] = {0};
	fill_frame(data, linesize);

	CVPixelBufferRef pixbuf = make_nv12_buffer();
	assert_int_equal(CVPixelBufferLockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	vt_copy_bytes_reset();
	vt_encode_submission_reset(1);

	assert_true(vt_encode_submission_reserve());
	assert_int_equal(vt_copy_frame_to_pixbuf(pixbuf, data, linesize), EXPECTED_BYTES);
	assert_int_equal(vt_copy_bytes_total(), 0);
	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	vt_encode_submission_cancel();
	assert_int_equal(vt_copy_bytes_total(), 0);
	assert_int_equal(vt_encode_submission_total(), 0);

	assert_true(vt_encode_submission_reserve());
	assert_int_equal(CVPixelBufferLockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	assert_int_equal(vt_copy_frame_to_pixbuf(pixbuf, data, linesize), EXPECTED_BYTES);
	assert_int_equal(CVPixelBufferUnlockBaseAddress(pixbuf, 0), kCVReturnSuccess);
	assert_int_equal(vt_copy_bytes_total(), 0);
	vt_encode_submission_commit();
	assert_int_equal(vt_copy_bytes_total(), EXPECTED_BYTES);
	assert_int_equal(vt_encode_submission_total(), 1);
	assert_false(vt_encode_submission_reserve());

	CVPixelBufferRelease(pixbuf);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_copies_nv12_1080p),
		cmocka_unit_test(test_stops_at_null_plane),
		cmocka_unit_test(test_submission_copy_bytes_commit_only_after_acceptance),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}

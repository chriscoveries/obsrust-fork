#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/bmem.h>
#include <util/c99defs.h>

#include "vaapi-probe-policy.h"

/* Startup spent seconds in the VAAPI encoder probe on NVIDIA machines: every
 * codec query opened every render node again, and each open of
 * nvidia-vaapi-driver (which cannot encode) initialised CUDA. */

static void nvidia_kernel_driver_is_decode_only(void **state)
{
	UNUSED_PARAMETER(state);
	assert_true(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/nvidia", NULL));
	assert_true(vaapi_driver_is_decode_only("nvidia", NULL));
}

static void other_kernel_drivers_are_probed(void **state)
{
	UNUSED_PARAMETER(state);
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/i915", NULL));
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/amdgpu", NULL));
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/nouveau", NULL));
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/nvidia-drm", NULL));
	assert_false(vaapi_driver_is_decode_only("", NULL));
	assert_false(vaapi_driver_is_decode_only(NULL, NULL));
}

static void libva_driver_override_is_probed(void **state)
{
	UNUSED_PARAMETER(state);
	/* LIBVA_DRIVER_NAME can name a VA driver that encodes on an NVIDIA GPU. */
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/nvidia", "nvenc"));
	assert_false(vaapi_driver_is_decode_only("../../../../bus/pci/drivers/nvidia", "nvidia"));
}

struct probe_count {
	int calls;
	uint32_t flags;
};

static uint32_t count_probe(const char *device_path, void *param)
{
	UNUSED_PARAMETER(device_path);
	struct probe_count *count = param;
	count->calls++;
	return count->flags;
}

static void each_device_is_probed_once(void **state)
{
	UNUSED_PARAMETER(state);
	struct vaapi_caps_cache cache = {0};
	struct probe_count count = {0, VAAPI_CODEC_H264 | VAAPI_CODEC_HEVC};

	/* The H.264, AV1 and HEVC queries for the same device. */
	for (int i = 0; i < 3; i++)
		assert_int_equal(vaapi_caps_cache_get(&cache, "/dev/dri/renderD128", count_probe, &count),
				 VAAPI_CODEC_H264 | VAAPI_CODEC_HEVC);
	assert_int_equal(count.calls, 1);
	assert_true(vaapi_caps_cache_has(&cache, "/dev/dri/renderD128"));

	count.flags = 0;
	assert_int_equal(vaapi_caps_cache_get(&cache, "/dev/dri/renderD129", count_probe, &count), 0);
	assert_int_equal(vaapi_caps_cache_get(&cache, "/dev/dri/renderD129", count_probe, &count), 0);
	assert_int_equal(count.calls, 2);

	vaapi_caps_cache_free(&cache);
	assert_false(vaapi_caps_cache_has(&cache, "/dev/dri/renderD128"));
}

static void added_answers_are_used_and_kept(void **state)
{
	UNUSED_PARAMETER(state);
	struct vaapi_caps_cache cache = {0};
	struct probe_count count = {0, VAAPI_CODEC_AV1};

	/* What the parallel probe of every render node found. */
	vaapi_caps_cache_add(&cache, "/dev/dri/renderD128", VAAPI_CODEC_H264);
	vaapi_caps_cache_add(&cache, "/dev/dri/renderD128", VAAPI_CODEC_AV1);
	assert_int_equal(vaapi_caps_cache_get(&cache, "/dev/dri/renderD128", count_probe, &count), VAAPI_CODEC_H264);
	assert_int_equal(count.calls, 0);

	vaapi_caps_cache_free(&cache);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(nvidia_kernel_driver_is_decode_only),
		cmocka_unit_test(other_kernel_drivers_are_probed),
		cmocka_unit_test(libva_driver_override_is_probed),
		cmocka_unit_test(each_device_is_probed_once),
		cmocka_unit_test(added_answers_are_used_and_kept),
	};

	int failed = cmocka_run_group_tests(tests, NULL, NULL);
	/* Every cached path is freed. */
	if (bnum_allocs() != 0)
		return 1;
	return failed;
}

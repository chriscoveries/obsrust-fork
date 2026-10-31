/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * An asynchronous texture consumer may retain frames after the GPU encode
 * thread has retired them.  Keep enough IOSurface-backed frames for the
 * VideoToolbox hardware encoder's observed fourteen-frame depth, plus two
 * slots so a completion does not immediately cause a repeat.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <cmocka.h>

#include "../../libobs/obs-internal.h"

#define VT_HARDWARE_ENCODER_IN_FLIGHT_FRAMES 14
#define ASYNC_TEXTURE_POOL_HEADROOM 2

static void async_texture_pool_covers_videotoolbox_in_flight_depth(void **state)
{
	(void)state;
	assert_true(NUM_ENCODE_TEXTURES >=
		    VT_HARDWARE_ENCODER_IN_FLIGHT_FRAMES + ASYNC_TEXTURE_POOL_HEADROOM);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(async_texture_pool_covers_videotoolbox_in_flight_depth),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

/******************************************************************************
    Copyright (C) 2026 by Chris Coveries

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "../../libobs/obs-texture-lease.h"

/* These tests cover the lease helper state machine. Production dispatch and
 * lifecycle coverage lives in test_encoder_error_stop. */

static void lease_helper_preserves_duplicate_frame_leases(void **state)
{
	(void)state;
	struct obs_texture_lease_state frame;
	struct obs_texture_lease_token submissions[3] = {0};
	obs_texture_lease_init(&frame);

	assert_true(obs_texture_lease_begin_frame(&frame));
	for (size_t i = 0; i < 3; i++)
		assert_true(obs_texture_lease_acquire(&frame, &submissions[i]));
	assert_int_equal(frame.outstanding, 3);
	assert_int_equal(obs_texture_lease_retire(&frame), OBS_TEXTURE_LEASE_HELD);
	assert_false(obs_texture_lease_is_drained(&frame));

	assert_int_equal(obs_texture_lease_release(&submissions[0]), OBS_TEXTURE_LEASE_HELD);
	assert_int_equal(obs_texture_lease_release(&submissions[1]), OBS_TEXTURE_LEASE_HELD);
	assert_int_equal(obs_texture_lease_release(&submissions[2]), OBS_TEXTURE_LEASE_RECYCLED);
	assert_true(obs_texture_lease_is_drained(&frame));
}

static void lease_helper_waits_for_out_of_order_consumers(void **state)
{
	(void)state;
	struct obs_texture_lease_state frame;
	struct obs_texture_lease_token submissions[3] = {0};
	obs_texture_lease_init(&frame);
	assert_true(obs_texture_lease_begin_frame(&frame));
	for (size_t i = 0; i < 3; i++)
		assert_true(obs_texture_lease_acquire(&frame, &submissions[i]));
	assert_int_equal(obs_texture_lease_retire(&frame), OBS_TEXTURE_LEASE_HELD);

	assert_int_equal(obs_texture_lease_release(&submissions[2]), OBS_TEXTURE_LEASE_HELD);
	assert_int_equal(obs_texture_lease_release(&submissions[0]), OBS_TEXTURE_LEASE_HELD);
	assert_int_equal(frame.outstanding, 1);
	assert_int_equal(obs_texture_lease_release(&submissions[1]), OBS_TEXTURE_LEASE_RECYCLED);
	assert_true(obs_texture_lease_is_drained(&frame));
}

static void lease_helper_reports_pool_exhaustion_policy(void **state)
{
	(void)state;
	struct obs_texture_lease_state frames[3];
	struct obs_texture_lease_token holds[3] = {0};
	for (size_t i = 0; i < 3; i++) {
		obs_texture_lease_init(&frames[i]);
		assert_true(obs_texture_lease_begin_frame(&frames[i]));
		assert_true(obs_texture_lease_acquire(&frames[i], &holds[i]));
		assert_int_equal(obs_texture_lease_retire(&frames[i]), OBS_TEXTURE_LEASE_HELD);
	}

	/* Every slot is still retired, so no slot can accept the newest frame. */
	for (size_t i = 0; i < 3; i++)
		assert_false(obs_texture_lease_begin_frame(&frames[i]));
	assert_int_equal(obs_texture_lease_pool_exhaustion(false), OBS_TEXTURE_POOL_DROP_NEWEST);
	assert_int_equal(obs_texture_lease_pool_exhaustion(true), OBS_TEXTURE_POOL_REPEAT_LAST);
	for (size_t i = 0; i < 3; i++)
		assert_int_equal(obs_texture_lease_release(&holds[i]), OBS_TEXTURE_LEASE_RECYCLED);
	assert_true(obs_texture_lease_begin_frame(&frames[0]));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(lease_helper_preserves_duplicate_frame_leases),
		cmocka_unit_test(lease_helper_waits_for_out_of_order_consumers),
		cmocka_unit_test(lease_helper_reports_pool_exhaustion_policy),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

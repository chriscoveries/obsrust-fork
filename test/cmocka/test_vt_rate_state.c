/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A live rate-control edit must re-arm the session whenever any field it sets changes. */
#include <stdio.h>
#include <VideoToolbox/VideoToolbox.h>
#include <obs-module.h>

#include "../../plugins/mac-videotoolbox/encoder.c"

static unsigned checks, failures;

static void check(bool yes, const char *message)
{
	++checks;
	if (!yes) {
		++failures;
		fprintf(stderr, "%s\n", message);
	}
}

int main(void)
{
	const struct vt_rate_state before = {
		.bitrate = 6000,
		.limit_bitrate = true,
		.max_bitrate = 6000,
		.max_bitrate_window = 1.5,
		.quality = 0.60f,
	};
	struct vt_rate_state after = before;
	check(!vt_rate_state_changed(&before, &after), "unchanged_rate_state_has_no_update: false positive");
	after.bitrate = 8000;
	check(vt_rate_state_changed(&before, &after), "bitrate_change_triggers_update: previously supported path");
	after = before;
	after.limit_bitrate = false;
	check(vt_rate_state_changed(&before, &after), "enable_switch_triggers_update: previously supported path");
	after = before;
	after.max_bitrate = 8000;
	check(vt_rate_state_changed(&before, &after), "max_bitrate_change_triggers_update: limit ignored");
	after = before;
	after.max_bitrate_window = 0.25;
	check(vt_rate_state_changed(&before, &after), "max_window_change_triggers_update: limit duration ignored");
	after = before;
	after.quality = 0.75f;
	check(vt_rate_state_changed(&before, &after), "quality_change_triggers_update: CRF quality ignored");
	after = before;
	check(!vt_rate_state_changed(&before, &after), "unchanged_rate_state_after_roundtrip: false positive");
	printf("VideoToolbox rate updates: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

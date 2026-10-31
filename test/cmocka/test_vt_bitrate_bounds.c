/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VideoToolbox AverageBitRate must not overflow 32-bit bits/sec arithmetic. */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>
#include <obs-module.h>

static unsigned checks, failures;
static unsigned rate_calls, quality_calls;
static int32_t actual_bps;
static float actual_quality;

static OSStatus capture_rate_property(VTSessionRef session, CFStringRef key, CFTypeRef value)
{
	(void)session;
	if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate) ||
	    CFEqual(key, kVTCompressionPropertyKey_ConstantBitRate)) {
		++rate_calls;
		if (!CFNumberGetValue(value, kCFNumberSInt32Type, &actual_bps)) {
			++failures;
			fprintf(stderr, "bitrate_property_conversion: unable to read CFNumber\n");
		}
	} else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) {
		++quality_calls;
		if (!CFNumberGetValue(value, kCFNumberFloat32Type, &actual_quality)) {
			++failures;
			fprintf(stderr, "quality_property_conversion: unable to read quality\n");
		}
	}
	return noErr;
}

#define VTSessionSetProperty capture_rate_property
#include "../../plugins/mac-videotoolbox/encoder.c"
#undef VTSessionSetProperty

static void check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		++failures;
		fprintf(stderr, "%s\n", message);
	}
}

static void test_abbr_bitrate(int kbps, bool valid)
{
	rate_calls = quality_calls = 0;
	actual_bps = -1;
	const OSStatus result = session_set_bitrate(NULL, "ABR", kbps, 0.60f, false, 6000, 1.5);
	if (valid) {
		check(result == noErr, "safe_bitrate_is_accepted: valid bitrate rejected");
		check(rate_calls == 1, "safe_bitrate_is_applied: expected one property setter call");
		check(actual_bps == (int32_t)((int64_t)kbps * 1000),
		      "safe_bitrate_has_exact_bits_per_second: scaled value incorrect");
	} else {
		check(result == kVTParameterErr,
		      "oversized_bitrate_rejected_before_conversion: invalid bitrate accepted");
		check(rate_calls == 0, "oversized_bitrate_must_not_submit: invalid bitrate reached VT property setter");
	}
	check(quality_calls == 0, "abr_does_not_configure_quality: CRF property unexpectedly submitted");
}

int main(void)
{
	test_abbr_bitrate(6000, true);
	test_abbr_bitrate(INT32_MAX / 1000, true);
	test_abbr_bitrate(INT32_MAX / 1000 + 1, false);
	test_abbr_bitrate(10000000, false); /* Existing GUI allows this maximum. */
	test_abbr_bitrate(-1, false);
	/* CRF on Apple Silicon only sets Quality; bitrate is not used in that mode. */
	is_apple_silicon = true;
	rate_calls = quality_calls = 0;
	const OSStatus crf_status = session_set_bitrate(NULL, "CRF", INT32_MAX, 0.65f, false, 6000, 1.5);
	check(crf_status == noErr, "crf_quality_path_preserved: quality-only configuration failed");
	check(quality_calls == 1 && rate_calls == 0, "crf_quality_does_not_scale_bitrate: unwanted overflow gate");
	check(actual_quality == 0.65f, "crf_quality_exact: submitted quality differs");
	printf("VideoToolbox bitrate bounds: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

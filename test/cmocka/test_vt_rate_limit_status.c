/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Headless test: the real VideoToolbox encoder TU with its property setter intercepted. */
#include <stdbool.h>
#include <stdio.h>
#include <VideoToolbox/VideoToolbox.h>
#include <obs-module.h>

static unsigned checks;
static unsigned failures;
static unsigned bitrate_calls;
static unsigned rate_limit_calls;
static OSStatus bitrate_result;
static OSStatus rate_limit_result;

static void check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		++failures;
		fprintf(stderr, "%s\n", message);
	}
}

static OSStatus capture_rate_property(VTSessionRef session, CFStringRef key, CFTypeRef value)
{
	(void)session;
	(void)value;
	if (CFEqual(key, kVTCompressionPropertyKey_DataRateLimits)) {
		++rate_limit_calls;
		return rate_limit_result;
	}
	if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
		++bitrate_calls;
		return bitrate_result;
	}
	return noErr;
}

#define VTSessionSetProperty capture_rate_property
#include "../../plugins/mac-videotoolbox/encoder.c"
#undef VTSessionSetProperty

static void check_result(const char *message, bool limited, OSStatus bitrate_status, OSStatus limit_status,
			 OSStatus expected_status, unsigned expected_bit_calls, unsigned expected_limit_calls)
{
	bitrate_result = bitrate_status;
	rate_limit_result = limit_status;
	bitrate_calls = 0;
	rate_limit_calls = 0;
	const OSStatus result = session_set_bitrate(NULL, "ABR", 6000, 0.6f, limited, 6000, 1.5);
	check(result == expected_status, message);
	check(bitrate_calls == expected_bit_calls, "bitrate_property_calls: wrong count");
	check(rate_limit_calls == expected_limit_calls, "rate_limit_property_calls: wrong count");
}

int main(void)
{
	check_result("data_rate_limit_error_is_not_masked: non-unsupported error was lost", true, noErr,
		     kVTParameterErr, kVTParameterErr, 1, 1);
	check_result("data_rate_limit_other_error_is_not_masked: OSStatus was replaced by success", true, noErr, -12905,
		     -12905, 1, 1);
	check_result("unsupported_rate_limit_is_nonfatal: old compatibility behaviour changed", true, noErr,
		     kVTPropertyNotSupportedErr, noErr, 1, 1);
	check_result("successful_rate_limit_returns_success: unexpected failure", true, noErr, noErr, noErr, 1, 1);
	check_result("bitrate_property_error_propagates: original setter error was lost", true, kVTParameterErr, noErr,
		     kVTParameterErr, 1, 0);
	check_result("disabled_limit_does_not_submit: unexpected rate-limit setter call", false, noErr, kVTParameterErr,
		     noErr, 1, 0);
	check_result("disabled_limit_preserves_bitrate_error: initial setter error was lost", false, kVTParameterErr,
		     noErr, kVTParameterErr, 1, 0);
	printf("videotoolbox rate status: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

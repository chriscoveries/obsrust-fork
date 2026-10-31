/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synchronous VideoToolbox submission failures must not retain pooled frames. */
#include <stdbool.h>
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <obs-module.h>

static unsigned failures, checks;
static unsigned allocations, encodes, locks, unlocks;
static CVPixelBufferRef inspected;
static int failure_stage; /* 0: no pool, 1: lock, 2: unlock, 3: encode, 4: accepted */

static CVPixelBufferPoolRef fake_pool(VTCompressionSessionRef s)
{
	(void)s;
	return failure_stage == 0 ? NULL : (CVPixelBufferPoolRef)kCFBooleanTrue;
}

static CVReturn tracked_create(CFAllocatorRef allocator, CVPixelBufferPoolRef pool, CVPixelBufferRef *out)
{
	(void)allocator;
	(void)pool;
	const CVReturn status = CVPixelBufferCreate(kCFAllocatorDefault, 16, 16,
						    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, out);
	if (status == kCVReturnSuccess) {
		++allocations;
		inspected = *out;
		CFRetain(inspected); /* Test owns its own reference, independent of encoder. */
	}
	return status;
}

static CVReturn fake_lock(CVPixelBufferRef buf, CVPixelBufferLockFlags flags)
{
	(void)buf;
	(void)flags;
	++locks;
	return failure_stage == 1 ? kCVReturnError : kCVReturnSuccess;
}

static CVReturn fake_unlock(CVPixelBufferRef buf, CVPixelBufferLockFlags flags)
{
	(void)buf;
	(void)flags;
	++unlocks;
	return failure_stage == 2 ? kCVReturnError : kCVReturnSuccess;
}

static OSStatus fake_encode(VTCompressionSessionRef session, CVImageBufferRef image, CMTime pts, CMTime duration,
			    CFDictionaryRef properties, void *source_ref, VTEncodeInfoFlags *flags)
{
	(void)session;
	(void)image;
	(void)pts;
	(void)duration;
	(void)properties;
	(void)flags;
	++encodes;
	if (source_ref != inspected) {
		++failures;
		fprintf(stderr, "frame_refcon_matches_buffer: callback ownership pointer changed\n");
	}
	return failure_stage == 3 ? kVTParameterErr : noErr;
}

static const void *fake_dequeue(CMSimpleQueueRef queue)
{
	(void)queue;
	return NULL;
}

#define VTCompressionSessionGetPixelBufferPool fake_pool
#define CVPixelBufferPoolCreatePixelBuffer tracked_create
#define CVPixelBufferLockBaseAddress fake_lock
#define CVPixelBufferUnlockBaseAddress fake_unlock
#define VTCompressionSessionEncodeFrame fake_encode
#define CMSimpleQueueDequeue fake_dequeue
#include "../../plugins/mac-videotoolbox/encoder.c"
#undef VTCompressionSessionGetPixelBufferPool
#undef CVPixelBufferPoolCreatePixelBuffer
#undef CVPixelBufferLockBaseAddress
#undef CVPixelBufferUnlockBaseAddress
#undef VTCompressionSessionEncodeFrame
#undef CMSimpleQueueDequeue

static void check(bool result, const char *name)
{
	++checks;
	if (!result) {
		++failures;
		fprintf(stderr, "%s\n", name);
	}
}

static void test_stage(int stage)
{
	failure_stage = stage;
	allocations = encodes = locks = unlocks = 0;
	inspected = NULL;
	struct vt_encoder encoder = {0};
	encoder.session = (VTCompressionSessionRef)kCFBooleanTrue;
	encoder.queue = (CMSimpleQueueRef)kCFBooleanTrue;
	encoder.colorspace = VIDEO_CS_709;
	encoder.fps_num = 30;
	encoder.fps_den = 1;
	struct encoder_frame frame = {0};
	struct encoder_packet packet = {0};
	bool received = false;

	const bool ok = vt_encode(&encoder, &frame, &packet, &received);
	check(ok == (stage == 4), "submission_stage_result: wrong success or failure");
	check(!received, "submission_stage_received: unexpected encoded packet");
	check(allocations == (stage == 0 ? 0u : 1u), "submission_stage_allocations: wrong count");
	check(encodes == (stage >= 3 ? 1u : 0u), "submission_stage_encodes: submitted at wrong stage");
	check(locks == (stage >= 1 ? 1u : 0u), "submission_stage_locks: wrong count");
	check(unlocks == (stage >= 2 ? 1u : 0u), "submission_stage_unlocks: wrong count");
	if (!inspected)
		return;

	const CFIndex held = CFGetRetainCount(inspected);
	if (stage == 4) {
		check(held == 2, "accepted_frame_retains_callback_ownership: premature release");
		/* Once accepted, the original reference belongs to the callback. */
		sample_encoded_callback(NULL, inspected, noErr, 0, NULL);
		check(CFGetRetainCount(inspected) == 1,
		      "callback_frees_accepted_frame: reference not released exactly once");
	} else {
		check(held == 1, "failed_frame_must_release_pool_reference: leaked source frame");
	}
	CFRelease(inspected);
	inspected = NULL;
}

int main(void)
{
	for (int stage = 0; stage < 5; ++stage)
		test_stage(stage);
	printf("VideoToolbox submission ownership: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

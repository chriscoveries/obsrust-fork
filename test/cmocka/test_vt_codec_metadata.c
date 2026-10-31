/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VideoToolbox's HLG HEVC Main10 encode on Apple silicon describes its
 * stream with a 24-byte configuration record, a "dvvC" atom in the sample
 * description extension atoms of the format description, next to "hvcC".
 * The encoder keeps the record of the format description it takes its
 * parameter sets from (the first keyframe's) and returns it through
 * get_codec_metadata for the MP4 muxer, which writes it as a dvvC box.
 *
 * The complete native encoder is included. The samples are built from a real
 * encode's hvcC and record (320x180, Apple M1 Max, macOS 27); no compression
 * session is created. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>

#include "../../plugins/mac-videotoolbox/encoder.c"

static unsigned checks;
static unsigned failures;

static void check(bool passed, const char *test, const char *assertion)
{
	checks++;
	if (!passed) {
		failures++;
		fprintf(stderr, "%s: %s\n", test, assertion);
	}
}

static const uint8_t hvcc[111] = {
	0x01, 0x02, 0x20, 0x00, 0x00, 0x00, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0xf0, 0x00, 0xfc,
	0xfd, 0xfa, 0xfa, 0x00, 0x00, 0x0b, 0x03, 0xa0, 0x00, 0x01, 0x00, 0x18, 0x40, 0x01, 0x0c, 0x01,
	0xff, 0xff, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
	0x3f, 0x0b, 0xc0, 0x90, 0xa1, 0x00, 0x01, 0x00, 0x29, 0x42, 0x01, 0x01, 0x02, 0x20, 0x00, 0x00,
	0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3f, 0xa0, 0x0a, 0x08, 0x0c, 0x1f,
	0x3b, 0x62, 0x02, 0xfb, 0x91, 0x64, 0x52, 0xff, 0xcb, 0x9f, 0xc4, 0xfe, 0xa6, 0xa1, 0x22, 0x41,
	0x20, 0x10, 0xa2, 0x00, 0x01, 0x00, 0x08, 0x44, 0x01, 0xc0, 0x72, 0xe1, 0x90, 0x53, 0x24,
};

/* Version 1.0, profile 8, level 1, RPU and base layer present, compatibility
 * ID 4 (HLG). */
static const uint8_t record[24] = {
	0x01, 0x00, 0x10, 0x0d, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* An HEVC format description with the hvcC atom and, when `dvvc` is set, a
 * dvvC atom of `dvvc_size` bytes. */
static CMFormatDescriptionRef format(const uint8_t *dvvc, size_t dvvc_size)
{
	CFDataRef h = CFDataCreate(NULL, hvcc, sizeof(hvcc));
	CFDataRef d = dvvc ? CFDataCreate(NULL, dvvc, (CFIndex)dvvc_size) : NULL;
	const void *atom_keys[2] = {CFSTR("hvcC"), CFSTR("dvvC")};
	const void *atom_values[2] = {h, d};
	CFDictionaryRef atoms = CFDictionaryCreate(NULL, atom_keys, atom_values, d ? 2 : 1,
						   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *keys[1] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
	const void *values[1] = {atoms};
	CFDictionaryRef extensions = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
							&kCFTypeDictionaryValueCallBacks);

	CMVideoFormatDescriptionRef fmt = NULL;
	OSStatus status = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_HEVC, 320, 180, extensions, &fmt);

	CFRelease(extensions);
	CFRelease(atoms);
	if (d)
		CFRelease(d);
	CFRelease(h);
	return status == noErr ? fmt : NULL;
}

/* A sample of `fmt` holding one length-prefixed slice NAL unit. */
static CMSampleBufferRef sample(CMFormatDescriptionRef fmt, bool keyframe, int64_t frame)
{
	uint8_t nal[8] = {0x00, 0x00, 0x00, 0x04, 0x02, 0x01, 0xd0, 0x5a};
	if (keyframe)
		nal[4] = 0x26;

	CMBlockBufferRef block = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, sizeof(nal), NULL, NULL, 0, sizeof(nal),
					       kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
		return NULL;
	CMBlockBufferReplaceDataBytes(nal, block, 0, sizeof(nal));

	CMSampleTimingInfo timing = {
		.duration = CMTimeMake(1, 30),
		.presentationTimeStamp = CMTimeMake(frame, 30),
		.decodeTimeStamp = kCMTimeInvalid,
	};
	size_t size = sizeof(nal);
	CMSampleBufferRef buffer = NULL;
	OSStatus status = CMSampleBufferCreateReady(NULL, block, fmt, 1, 1, &timing, 1, &size, &buffer);
	CFRelease(block);
	if (status != noErr)
		return NULL;

	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, true);
	CFMutableDictionaryRef attachment = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
	CFDictionarySetValue(attachment, kCMSampleAttachmentKey_DependsOnOthers,
			     keyframe ? kCFBooleanFalse : kCFBooleanTrue);
	return buffer;
}

static struct vt_encoder *hevc_encoder(void)
{
	struct vt_encoder *enc = bzalloc(sizeof(*enc));
	enc->codec_type = kCMVideoCodecType_HEVC;
	enc->fps_num = 30;
	enc->fps_den = 1;
	return enc;
}

/* Hands one sample of `fmt` to parse_sample(), which takes its reference. */
static bool parse(struct vt_encoder *enc, CMFormatDescriptionRef fmt, bool keyframe, int64_t frame)
{
	CMSampleBufferRef buffer = sample(fmt, keyframe, frame);
	struct encoder_packet packet = {0};
	return buffer && parse_sample(enc, buffer, &packet, CMTimeMake(2, 30));
}

static bool has_extra_data(struct vt_encoder *enc)
{
	uint8_t *data = NULL;
	size_t size = 0;
	return vt_extra_data(enc, &data, &size) && size > 0;
}

static void first_keyframe_record_is_returned(void)
{
	const char *test = "first_keyframe_record_is_returned";
	struct vt_encoder *enc = hevc_encoder();
	CMFormatDescriptionRef fmt = format(record, sizeof(record));
	check(fmt != NULL, test, "could not build the format description");
	if (!fmt) {
		vt_destroy(enc);
		return;
	}

	uint8_t *data = NULL;
	size_t size = 0;
	check(!vt_codec_metadata(enc, &data, &size), test, "a record before the first sample");

	check(parse(enc, fmt, true, 0), test, "parse_sample failed on the keyframe");
	check(has_extra_data(enc), test, "the keyframe gave no parameter sets");
	check(vt_codec_metadata(enc, &data, &size), test, "no record after the first keyframe");
	check(size == sizeof(record) && data && memcmp(data, record, sizeof(record)) == 0, test,
	      "the record is not the format description's dvvC atom");

	check(parse(enc, fmt, false, 1), test, "parse_sample failed on a later frame");
	check(vt_codec_metadata(enc, &data, &size) && size == sizeof(record), test, "a later frame lost the record");

	CFRelease(fmt);
	vt_destroy(enc);
}

/* An SDR or PQ encode has no dvvC atom: no record, as before. */
static void no_atom_no_record(void)
{
	const char *test = "no_atom_no_record";
	struct vt_encoder *enc = hevc_encoder();
	CMFormatDescriptionRef fmt = format(NULL, 0);
	check(fmt != NULL, test, "could not build the format description");
	if (!fmt) {
		vt_destroy(enc);
		return;
	}

	check(parse(enc, fmt, true, 0), test, "parse_sample failed on the keyframe");
	check(has_extra_data(enc), test, "the keyframe gave no parameter sets");
	uint8_t *data = NULL;
	size_t size = 0;
	check(!vt_codec_metadata(enc, &data, &size), test, "a record without a dvvC atom");

	CFRelease(fmt);
	vt_destroy(enc);
}

static void empty_atom_no_record(void)
{
	const char *test = "empty_atom_no_record";
	struct vt_encoder *enc = hevc_encoder();
	CMFormatDescriptionRef fmt = format(record, 0);
	check(fmt != NULL, test, "could not build the format description");
	if (!fmt) {
		vt_destroy(enc);
		return;
	}

	check(parse(enc, fmt, true, 0), test, "parse_sample failed on the keyframe");
	uint8_t *data = NULL;
	size_t size = 0;
	check(!vt_codec_metadata(enc, &data, &size), test, "an empty dvvC atom reported as a record");

	CFRelease(fmt);
	vt_destroy(enc);
}

/* The record stays the one of the description the parameter sets came from:
 * a later keyframe with another record does not replace it. */
static void later_keyframe_keeps_first_record(void)
{
	const char *test = "later_keyframe_keeps_first_record";
	uint8_t other[sizeof(record)];
	memcpy(other, record, sizeof(record));
	other[3] ^= 0x01;

	struct vt_encoder *enc = hevc_encoder();
	CMFormatDescriptionRef first = format(record, sizeof(record));
	CMFormatDescriptionRef later = format(other, sizeof(other));
	check(first && later, test, "could not build the format descriptions");
	if (!first || !later) {
		if (first)
			CFRelease(first);
		if (later)
			CFRelease(later);
		vt_destroy(enc);
		return;
	}

	check(parse(enc, first, true, 0), test, "parse_sample failed on the first keyframe");
	check(parse(enc, later, true, 60), test, "parse_sample failed on the later keyframe");
	uint8_t *data = NULL;
	size_t size = 0;
	check(vt_codec_metadata(enc, &data, &size), test, "no record after two keyframes");
	check(size == sizeof(record) && data && memcmp(data, record, sizeof(record)) == 0, test,
	      "a later keyframe replaced the first record");

	CFRelease(first);
	CFRelease(later);
	vt_destroy(enc);
}

/* vt_destroy() frees the record with the rest of the encoder. */
static void record_freed_with_encoder(void)
{
	const char *test = "record_freed_with_encoder";
	CMFormatDescriptionRef fmt = format(record, sizeof(record));
	check(fmt != NULL, test, "could not build the format description");
	if (!fmt)
		return;

	long before = bnum_allocs();
	struct vt_encoder *enc = hevc_encoder();
	check(parse(enc, fmt, true, 0), test, "parse_sample failed on the keyframe");
	vt_destroy(enc);
	check(bnum_allocs() == before, test, "the encoder's allocations outlive vt_destroy");

	CFRelease(fmt);
}

int main(void)
{
	first_keyframe_record_is_returned();
	no_atom_no_record();
	empty_atom_no_record();
	later_keyframe_keeps_first_record();
	record_freed_with_encoder();
	printf("videotoolbox codec metadata: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

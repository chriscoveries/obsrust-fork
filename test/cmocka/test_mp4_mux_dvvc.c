/* HLG-compatible HDR signaling in Hybrid MP4/MOV recordings.
 *
 * On a Mac, VideoToolbox encodes HLG HEVC Main10 as HEVC profile 8.4: the
 * picture is standard HLG, and every frame also carries dynamic HDR metadata
 * (NAL unit type 62). The encoder describes that stream with a 24-byte
 * configuration record in its format description. Players use the dynamic
 * metadata only when the hvc1 sample entry carries the record, as a 'dvvC'
 * box after 'hvcC'; without it the recording plays as plain HLG.
 *
 * With MP4_WRITE_HLG_COMPATIBLE_HDR_CONFIG (the write_hlg_compatible_hdr_config
 * muxer option), mp4-mux.c writes the record obs_encoder_get_codec_metadata()
 * returns, verbatim, as that box. It asks for the record when it writes the
 * sample description, because an encoder has it only after its first packet.
 * Without the flag, or without a well-formed record, the sample entry is the
 * same as before.
 *
 * The test builds obs-outputs' mp4-output.c and mp4-mux.c into the binary,
 * muxes a few packets from stub HEVC encoders (a real VideoToolbox encode's
 * parameter sets and record, no encoding) into a file, and reads the hvc1
 * sample entry back. obs_startup() needs a display on Linux, so those cases
 * skip without one (run under xvfb-run). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <util/base.h>
#include <util/bmem.h>
#include <util/file-serializer.h>

#include "mp4-output.c.h"

#define OUTPUT_ID "mp4_mux_dvvc_output"
#define WIDTH 320
#define HEIGHT 180
#define FPS 30

/* mp4-mux.c and mp4-output.c name things through the module's locale. */
const char *obs_module_text(const char *val)
{
	return val;
}

static bool obs_up;
static video_t *video;

static void quiet_log(int lvl, const char *msg, va_list args, void *p)
{
	(void)p;
	if (lvl <= LOG_WARNING) {
		vfprintf(stderr, msg, args);
		fputc('\n', stderr);
	}
}

/* ------------------------------------------------------------------------ */
/* A VideoToolbox HLG HEVC Main10 encode at 320x180 (Apple M1 Max, macOS 27) */

/* VPS, SPS and PPS as the encoder's get_extra_data returns them (Annex-B). */
static const uint8_t parameter_sets[] = {
	0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0xb0,
	0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3f, 0x0b, 0xc0, 0x90, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01,
	0x01, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3f, 0xa0,
	0x0a, 0x08, 0x0c, 0x1f, 0x3b, 0x62, 0x02, 0xfb, 0x91, 0x64, 0x52, 0xff, 0xcb, 0x9f, 0xc4, 0xfe, 0xa6,
	0xa1, 0x22, 0x41, 0x20, 0x10, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc0, 0x72, 0xe1, 0x90, 0x53, 0x24,
};

/* The "dvvC" atom of the same encode's format description: version 1.0,
 * profile 8, level 1, RPU and base layer present, compatibility ID 4 (HLG). */
static const uint8_t record[24] = {
	0x01, 0x00, 0x10, 0x0d, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ------------------------------------------------------------------------ */
/* Stub encoders: parameter sets and a record, never encode.                */

enum record_mode {
	RECORD_NONE,        /* no get_codec_metadata callback */
	RECORD_VALID,       /* the 24-byte record */
	RECORD_UNAVAILABLE, /* the callback reports no record */
	RECORD_NULL,        /* reports a record without a pointer */
	RECORD_EMPTY,       /* reports a record of size 0 */
	RECORD_SHORT,       /* reports 23 bytes */
	RECORD_LONG,        /* reports 25 bytes */
};

struct stub_type {
	const char *id;
	enum record_mode mode;
	/* registration size, 0 for sizeof(struct obs_encoder_info) */
	size_t info_size;
};

static const struct stub_type stub_valid = {"dvvc_valid", RECORD_VALID, 0};
static const struct stub_type stub_none = {"dvvc_none", RECORD_NONE, 0};
static const struct stub_type stub_unavailable = {"dvvc_unavailable", RECORD_UNAVAILABLE, 0};
static const struct stub_type stub_null = {"dvvc_null", RECORD_NULL, 0};
static const struct stub_type stub_empty = {"dvvc_empty", RECORD_EMPTY, 0};
static const struct stub_type stub_short = {"dvvc_short", RECORD_SHORT, 0};
static const struct stub_type stub_long = {"dvvc_long", RECORD_LONG, 0};
/* A plugin built against the headers before get_codec_metadata: its struct
 * ends at request_keyframe. Its callback lies past the size it registers. */
static const struct stub_type stub_old_size = {"dvvc_old_size", RECORD_VALID,
					       offsetof(struct obs_encoder_info, get_codec_metadata)};

struct stub_encoder {
	const struct stub_type *type;
	uint8_t record[sizeof(record) + 1];
};

/* Like VideoToolbox, a stub has its record only after its first packet:
 * the cases set this once the muxer exists. */
static bool record_ready;
static unsigned metadata_calls;

static const char *stub_name(void *type_data)
{
	return ((const struct stub_type *)type_data)->id;
}

static void *stub_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	(void)settings;
	struct stub_encoder *e = bzalloc(sizeof(*e));
	const struct stub_type *all[] = {&stub_valid, &stub_none,  &stub_unavailable, &stub_null,
					 &stub_empty, &stub_short, &stub_long,        &stub_old_size};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (strcmp(all[i]->id, obs_encoder_get_id(encoder)) == 0)
			e->type = all[i];
	}
	memcpy(e->record, record, sizeof(record));
	return e;
}

static void stub_destroy(void *data)
{
	bfree(data);
}

static bool stub_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
	(void)data;
	(void)frame;
	(void)packet;
	*received = false;
	return true;
}

static bool stub_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	(void)data;
	*extra_data = (uint8_t *)parameter_sets;
	*size = sizeof(parameter_sets);
	return true;
}

static bool stub_codec_metadata(void *data, uint8_t **metadata, size_t *size)
{
	struct stub_encoder *e = data;
	metadata_calls++;

	if (!record_ready || e->type->mode == RECORD_UNAVAILABLE)
		return false;

	*metadata = e->record;
	*size = sizeof(record);
	if (e->type->mode == RECORD_NULL)
		*metadata = NULL;
	else if (e->type->mode == RECORD_EMPTY)
		*size = 0;
	else if (e->type->mode == RECORD_SHORT)
		*size = sizeof(record) - 1;
	else if (e->type->mode == RECORD_LONG)
		*size = sizeof(record) + 1;
	return true;
}

static void register_stub(const struct stub_type *type)
{
	struct obs_encoder_info info = {
		.id = type->id,
		.type = OBS_ENCODER_VIDEO,
		.codec = "hevc",
		.get_name = stub_name,
		.create = stub_create,
		.destroy = stub_destroy,
		.encode = stub_encode,
		.get_extra_data = stub_extra_data,
		.get_codec_metadata = type->mode == RECORD_NONE ? NULL : stub_codec_metadata,
		.type_data = (void *)type,
	};
	obs_register_encoder_s(&info, type->info_size ? type->info_size : sizeof(info));
}

/* ------------------------------------------------------------------------ */
/* Output that only holds the encoder for the muxer.                        */

static const char *output_name(void *type_data)
{
	(void)type_data;
	return "mp4 mux dvvc test output";
}

static void *output_create(obs_data_t *settings, obs_output_t *output)
{
	(void)settings;
	return output;
}

static void output_destroy(void *data)
{
	(void)data;
}

static bool output_start(void *data)
{
	(void)data;
	return false;
}

static void output_stop(void *data, uint64_t ts)
{
	(void)data;
	(void)ts;
}

static void output_encoded_packet(void *data, struct encoder_packet *packet)
{
	(void)data;
	(void)packet;
}

/* An initialized encoder of `type` on the test video, held by an output. */
static obs_encoder_t *create_encoder(const struct stub_type *type, obs_output_t **output)
{
	obs_encoder_t *enc = obs_video_encoder_create(type->id, "video", NULL, NULL);
	assert_non_null(enc);
	obs_encoder_set_video(enc, video);

	*output = obs_output_create(OUTPUT_ID, "output", NULL, NULL);
	assert_non_null(*output);
	obs_output_set_video_encoder(*output, enc);
	assert_true(obs_output_initialize_encoders(*output, 0));
	return enc;
}

/* ------------------------------------------------------------------------ */
/* Box reading                                                              */

static uint32_t rb32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Finds the first box named `name` among the children in [start, end) and
 * returns its offset, or 0 (no box we look for starts the file). */
static size_t find_box(const uint8_t *buf, size_t start, size_t end, const char *name, size_t *box_end)
{
	size_t off = start;
	while (off + 8 <= end) {
		size_t size = rb32(buf + off);
		if (size < 8 || off + size > end)
			return 0;
		if (memcmp(buf + off + 4, name, 4) == 0) {
			*box_end = off + size;
			return off;
		}
		off += size;
	}
	return 0;
}

/* The child boxes of an hvc1 sample entry start after its 8-byte header, the
 * 8-byte SampleEntry and the 70-byte VisualSampleEntry fields
 * (ISO/IEC 14496-12 12.1.3). */
#define HVC1_CHILDREN 86

struct child {
	char type[5];
	size_t offset;
	size_t size;
};

struct hvc1 {
	uint8_t *bytes; /* the whole hvc1 box */
	size_t size;
	bool fragmented; /* read from a moov with mvex */
	struct child children[8];
	size_t count;
};

static void read_children(struct hvc1 *entry)
{
	size_t off = HVC1_CHILDREN;
	entry->count = 0;
	while (off + 8 <= entry->size && entry->count < sizeof(entry->children) / sizeof(entry->children[0])) {
		struct child *c = &entry->children[entry->count++];
		c->offset = off;
		c->size = rb32(entry->bytes + off);
		memcpy(c->type, entry->bytes + off + 4, 4);
		c->type[4] = 0;
		if (c->size < 8 || off + c->size > entry->size) {
			fprintf(stderr, "hvc1: child '%s' at %zu has size %zu past the entry\n", c->type, off, c->size);
			fail();
		}
		off += c->size;
	}
}

/* Copies the hvc1 sample entry of the first moov of the file at `path`. */
static void read_hvc1(const char *path, struct hvc1 *entry)
{
	FILE *f = os_fopen(path, "rb");
	assert_non_null(f);
	fseek(f, 0, SEEK_END);
	size_t size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = bmalloc(size);
	size_t got = fread(buf, 1, size, f);
	fclose(f);
	os_unlink(path);
	assert_int_equal(got, size);

	const char *boxes[] = {"moov", "trak", "mdia", "minf", "stbl", "stsd"};
	size_t start = 0, end = size, box_end = 0, moov_end = 0;
	size_t moov = find_box(buf, 0, size, "moov", &moov_end);
	for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
		size_t off = find_box(buf, start, end, boxes[i], &box_end);
		if (!off) {
			fprintf(stderr, "no %s box in %s\n", boxes[i], path);
			bfree(buf);
			fail();
		}
		start = off + 8;
		end = box_end;
	}

	/* stsd: version, flags, entry_count, then the sample entry */
	size_t sample_entry = start + 8;
	size_t entry_size = rb32(buf + sample_entry);
	assert_true(sample_entry + entry_size <= end);
	assert_memory_equal(buf + sample_entry + 4, "hvc1", 4);

	size_t mvex_end;
	entry->fragmented = find_box(buf, moov + 8, moov_end, "mvex", &mvex_end) != 0;
	entry->size = entry_size;
	entry->bytes = bmalloc(entry_size);
	memcpy(entry->bytes, buf + sample_entry, entry_size);
	bfree(buf);
	read_children(entry);
}

static const struct child *find_child(const struct hvc1 *entry, const char *type)
{
	for (size_t i = 0; i < entry->count; i++) {
		if (strcmp(entry->children[i].type, type) == 0)
			return &entry->children[i];
	}
	return NULL;
}

static void print_children(const char *label, const struct hvc1 *entry)
{
	fprintf(stderr, "%s: hvc1 %zu bytes:", label, entry->size);
	for (size_t i = 0; i < entry->count; i++)
		fprintf(stderr, " %s(%zu)", entry->children[i].type, entry->children[i].size);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------------ */
/* Muxing                                                                   */

/* One slice NAL unit: an IDR picture (type 19) or a trailing one (type 1).
 * The muxer stores it as it is; nothing decodes it. */
static void submit_frame(struct mp4_mux *mux, obs_encoder_t *enc, int64_t frame)
{
	const bool keyframe = frame % FPS == 0;
	uint8_t payload[] = {0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xaf, 0x5a, 0x5a, 0x5a};
	if (keyframe)
		payload[4] = 0x26;

	/* refcounted packet data, as obs_encoder_packet_create_instance makes */
	long *refs = bmalloc(sizeof(long) + sizeof(payload));
	*refs = 1;
	memcpy(refs + 1, payload, sizeof(payload));

	struct encoder_packet pkt = {
		.data = (uint8_t *)(refs + 1),
		.size = sizeof(payload),
		.pts = frame,
		.dts = frame,
		.timebase_num = 1,
		.timebase_den = FPS,
		.type = OBS_ENCODER_VIDEO,
		.keyframe = keyframe,
		.encoder = enc,
	};
	pkt.dts_usec = frame * 1000000 / FPS;
	pkt.sys_dts_usec = pkt.dts_usec;

	assert_true(mp4_mux_submit_packet(mux, &pkt));
	obs_encoder_packet_release(&pkt);
}

/* Muxes two seconds from a `type` encoder with `flags` into a file (two
 * fragments: the first moov is written at the second keyframe) and reads
 * its hvc1 sample entry back. */
static void mux_hvc1(const struct stub_type *type, int flags, struct hvc1 *entry)
{
	obs_output_t *output;
	obs_encoder_t *enc = create_encoder(type, &output);

	char path[64];
	snprintf(path, sizeof(path), "test_mp4_mux_dvvc_%s.mp4", type->id);
	struct serializer s;
	assert_true(file_output_serializer_init(&s, path));

	record_ready = false;
	struct mp4_mux *mux = mp4_mux_create(output, &s, (enum mp4_mux_flags)flags, FLAVOR_MP4);
	assert_non_null(mux);
	record_ready = true;

	for (int64_t i = 0; i < 2 * FPS + 2; i++)
		submit_frame(mux, enc, i);
	assert_true(mp4_mux_finalise(mux));
	mp4_mux_destroy(mux);
	file_output_serializer_free(&s);

	obs_output_release(output);
	obs_encoder_release(enc);

	read_hvc1(path, entry);
}

/* The entry with the flag set is the entry without it plus the dvvC box
 * right after hvcC, carrying the record byte for byte. */
static void check_record_after_hvcC(const struct hvc1 *with, const struct hvc1 *without)
{
	print_children("flag set", with);
	print_children("flag clear", without);

	assert_true(with->count >= 2);
	assert_string_equal(with->children[0].type, "hvcC");
	if (strcmp(with->children[1].type, "dvvC") != 0) {
		fprintf(stderr, "no dvvC box after hvcC: the record did not reach the file\n");
		fail();
	}

	const struct child *dvvc = &with->children[1];
	assert_int_equal(dvvc->size, 8 + sizeof(record));
	assert_memory_equal(with->bytes + dvvc->offset + 8, record, sizeof(record));
	assert_null(find_child(without, "dvvC"));

	/* Everything else, the hvc1 box size aside, is unchanged. */
	assert_int_equal(with->size, without->size + dvvc->size);
	assert_memory_equal(with->bytes + 4, without->bytes + 4, dvvc->offset - 4);
	assert_memory_equal(with->bytes + dvvc->offset + dvvc->size, without->bytes + dvvc->offset,
			    without->size - dvvc->offset);
}

static void free_entry(struct hvc1 *entry)
{
	bfree(entry->bytes);
	entry->bytes = NULL;
}

/* ------------------------------------------------------------------------ */
/* Cases                                                                    */

/* The record reaches the final moov, after hvcC, though the encoder had none
 * when the muxer was created. */
static void record_written_after_hvcC(void **state)
{
	(void)state;
	if (!obs_up)
		skip();

	struct hvc1 with, without;
	mux_hvc1(&stub_valid, MP4_USE_NEGATIVE_CTS | MP4_WRITE_HLG_COMPATIBLE_HDR_CONFIG, &with);
	mux_hvc1(&stub_valid, MP4_USE_NEGATIVE_CTS, &without);
	assert_false(with.fragmented);
	check_record_after_hvcC(&with, &without);
	free_entry(&with);
	free_entry(&without);
}

/* A recording left fragmented (skip_soft_remux) has only the moov written at
 * the first fragment; it carries the record too. */
static void record_in_fragmented_moov(void **state)
{
	(void)state;
	if (!obs_up)
		skip();

	struct hvc1 with, without;
	mux_hvc1(&stub_valid, MP4_USE_NEGATIVE_CTS | MP4_SKIP_FINALISATION | MP4_WRITE_HLG_COMPATIBLE_HDR_CONFIG,
		 &with);
	mux_hvc1(&stub_valid, MP4_USE_NEGATIVE_CTS | MP4_SKIP_FINALISATION, &without);
	assert_true(with.fragmented);
	check_record_after_hvcC(&with, &without);
	free_entry(&with);
	free_entry(&without);
}

/* With the flag set, an encoder without a well-formed record leaves the
 * sample entry exactly as without the flag. */
static void check_entry_unchanged(const struct stub_type *type)
{
	struct hvc1 with, without;
	mux_hvc1(type, MP4_USE_NEGATIVE_CTS | MP4_WRITE_HLG_COMPATIBLE_HDR_CONFIG, &with);
	mux_hvc1(type, MP4_USE_NEGATIVE_CTS, &without);
	print_children(type->id, &with);

	assert_null(find_child(&with, "dvvC"));
	assert_int_equal(with.size, without.size);
	assert_memory_equal(with.bytes, without.bytes, with.size);
	assert_string_equal(with.children[0].type, "hvcC");
	assert_string_equal(with.children[1].type, "colr");
	free_entry(&with);
	free_entry(&without);
}

static void no_callback_no_box(void **state)
{
	(void)state;
	if (!obs_up)
		skip();
	check_entry_unchanged(&stub_none);
}

static void no_record_no_box(void **state)
{
	(void)state;
	if (!obs_up)
		skip();
	check_entry_unchanged(&stub_unavailable);
}

/* A record reported without data, or not of the 24 bytes the configuration
 * record has, is not written. */
static void malformed_record_no_box(void **state)
{
	(void)state;
	if (!obs_up)
		skip();
	check_entry_unchanged(&stub_null);
	check_entry_unchanged(&stub_empty);
	check_entry_unchanged(&stub_short);
	check_entry_unchanged(&stub_long);
}

/* A plugin registered with the struct size before get_codec_metadata gets no
 * callback copied, so it is never called and nothing is written. */
static void old_struct_size_no_box(void **state)
{
	(void)state;
	if (!obs_up)
		skip();
	metadata_calls = 0;
	check_entry_unchanged(&stub_old_size);
	assert_int_equal(metadata_calls, 0);
}

/* obs_encoder_get_codec_metadata() passes the encoder's record through, and
 * reports none for an encoder that has none or reports one without data. */
static void accessor_passes_record_through(void **state)
{
	(void)state;
	if (!obs_up)
		skip();

	obs_output_t *output;
	obs_encoder_t *enc = create_encoder(&stub_valid, &output);
	record_ready = true;

	uint8_t *data = NULL;
	size_t size = 0;
	if (!obs_encoder_get_codec_metadata(enc, &data, &size)) {
		fprintf(stderr, "obs_encoder_get_codec_metadata() returned false for an encoder with a record\n");
		fail();
	}
	assert_int_equal(size, sizeof(record));
	assert_memory_equal(data, record, sizeof(record));

	record_ready = false;
	assert_false(obs_encoder_get_codec_metadata(enc, &data, &size));
	obs_output_release(output);
	obs_encoder_release(enc);
}

static void accessor_rejects_missing_data(void **state)
{
	(void)state;
	if (!obs_up)
		skip();

	const struct stub_type *types[] = {&stub_none, &stub_unavailable, &stub_null, &stub_empty, &stub_old_size};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		obs_output_t *output;
		obs_encoder_t *enc = create_encoder(types[i], &output);
		record_ready = true;

		uint8_t *data = NULL;
		size_t size = 0;
		if (obs_encoder_get_codec_metadata(enc, &data, &size)) {
			fprintf(stderr, "%s: obs_encoder_get_codec_metadata() returned true (%p, %zu)\n", types[i]->id,
				(void *)data, size);
			fail();
		}
		obs_output_release(output);
		obs_encoder_release(enc);
	}

	uint8_t *data = NULL;
	size_t size = 0;
	assert_false(obs_encoder_get_codec_metadata(NULL, &data, &size));
}

/* The custom muxer option sets and clears the flag; it is off by default. */
static void option_sets_flag(void **state)
{
	(void)state;
	struct mp4_output out;
	memset(&out, 0, sizeof(out));

	parse_custom_options(&out, "");
	assert_int_equal(out.flags, MP4_USE_NEGATIVE_CTS);

	parse_custom_options(&out, "write_hlg_compatible_hdr_config=1");
	assert_int_equal(out.flags, MP4_USE_NEGATIVE_CTS | MP4_WRITE_HLG_COMPATIBLE_HDR_CONFIG);

	parse_custom_options(&out, "write_hlg_compatible_hdr_config=0");
	assert_int_equal(out.flags, MP4_USE_NEGATIVE_CTS);
}

static int setup(void **state)
{
	(void)state;
	base_set_log_handler(quiet_log, NULL);
	obs_up = obs_startup("en-US", NULL, NULL);
	if (!obs_up) {
		fprintf(stderr, "skipped: obs_startup failed (no display?)\n");
		return 0;
	}

	/* The encoders' video: no graphics, nothing is rendered. */
	struct video_output_info voi = {
		.name = "dvvc test video",
		.format = VIDEO_FORMAT_P010,
		.fps_num = FPS,
		.fps_den = 1,
		.width = WIDTH,
		.height = HEIGHT,
		.cache_size = 4,
		.colorspace = VIDEO_CS_2100_HLG,
		.range = VIDEO_RANGE_PARTIAL,
	};
	assert_int_equal(video_output_open(&video, &voi), VIDEO_OUTPUT_SUCCESS);

	struct obs_output_info info = {
		.id = OUTPUT_ID,
		.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
		.get_name = output_name,
		.create = output_create,
		.destroy = output_destroy,
		.start = output_start,
		.stop = output_stop,
		.encoded_packet = output_encoded_packet,
		.encoded_video_codecs = "hevc",
	};
	obs_register_output(&info);

	register_stub(&stub_valid);
	register_stub(&stub_none);
	register_stub(&stub_unavailable);
	register_stub(&stub_null);
	register_stub(&stub_empty);
	register_stub(&stub_short);
	register_stub(&stub_long);
	register_stub(&stub_old_size);
	return 0;
}

static int teardown(void **state)
{
	(void)state;
	if (obs_up) {
		video_output_close(video);
		obs_shutdown();
	}
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(record_written_after_hvcC),
		cmocka_unit_test(record_in_fragmented_moov),
		cmocka_unit_test(no_callback_no_box),
		cmocka_unit_test(no_record_no_box),
		cmocka_unit_test(malformed_record_no_box),
		cmocka_unit_test(old_struct_size_no_box),
		cmocka_unit_test(accessor_passes_record_through),
		cmocka_unit_test(accessor_rejects_missing_data),
		cmocka_unit_test(option_sets_flag),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}

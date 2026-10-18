#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>

#include "obs-preflight-collector.h"
#include "preflight_graphics_test_utils.h"

#define FPS 5
#define WINDOW_MS 500
#define SAMPLE_INTERVAL_MS 250
#define SAMPLE_COUNT 12

#define ASSERT_OFFSET(type, field, expected) \
	_Static_assert(offsetof(type, field) == expected, #type "." #field " ABI offset")

_Static_assert(sizeof(struct obs_preflight_sample) == 96, "sample ABI size");
_Static_assert(_Alignof(struct obs_preflight_sample) == 8, "sample ABI alignment");
ASSERT_OFFSET(struct obs_preflight_sample, timestamp_ms, 0);
ASSERT_OFFSET(struct obs_preflight_sample, average_frame_time_ns, 8);
ASSERT_OFFSET(struct obs_preflight_sample, frame_interval_ns, 16);
ASSERT_OFFSET(struct obs_preflight_sample, lagged_frames, 24);
ASSERT_OFFSET(struct obs_preflight_sample, output_total_bytes, 32);
ASSERT_OFFSET(struct obs_preflight_sample, output_total_frames, 40);
ASSERT_OFFSET(struct obs_preflight_sample, output_frames_dropped, 48);
ASSERT_OFFSET(struct obs_preflight_sample, output_congestion, 56);
ASSERT_OFFSET(struct obs_preflight_sample, connect_time_ms, 64);
ASSERT_OFFSET(struct obs_preflight_sample, encoder_total_frames, 72);
ASSERT_OFFSET(struct obs_preflight_sample, encoder_skipped_frames, 80);
ASSERT_OFFSET(struct obs_preflight_sample, cpu_percent, 88);

_Static_assert(sizeof(struct obs_preflight_window) == 416, "window ABI size");
_Static_assert(_Alignof(struct obs_preflight_window) == 8, "window ABI alignment");
ASSERT_OFFSET(struct obs_preflight_window, start_ms, 0);
ASSERT_OFFSET(struct obs_preflight_window, end_ms, 8);
ASSERT_OFFSET(struct obs_preflight_window, duration_ms, 16);
ASSERT_OFFSET(struct obs_preflight_window, output_bitrate_kbps, 24);
ASSERT_OFFSET(struct obs_preflight_window, output_bytes, 32);
ASSERT_OFFSET(struct obs_preflight_window, output_frames, 40);
ASSERT_OFFSET(struct obs_preflight_window, output_frames_dropped, 48);
ASSERT_OFFSET(struct obs_preflight_window, output_drop_percent, 56);
ASSERT_OFFSET(struct obs_preflight_window, encoder_frames, 64);
ASSERT_OFFSET(struct obs_preflight_window, encoder_frames_skipped, 72);
ASSERT_OFFSET(struct obs_preflight_window, encoder_skip_percent, 80);
ASSERT_OFFSET(struct obs_preflight_window, lagged_frames, 88);
ASSERT_OFFSET(struct obs_preflight_window, render_lag_percent, 96);
ASSERT_OFFSET(struct obs_preflight_window, average_frame_time_ns, 104);
ASSERT_OFFSET(struct obs_preflight_window, frame_interval_ns, 112);
ASSERT_OFFSET(struct obs_preflight_window, render_headroom_ns, 120);
ASSERT_OFFSET(struct obs_preflight_window, congestion_percent, 128);
ASSERT_OFFSET(struct obs_preflight_window, connect_time_ms, 136);
ASSERT_OFFSET(struct obs_preflight_window, cpu_percent, 144);
ASSERT_OFFSET(struct obs_preflight_window, verdict, 152);
ASSERT_OFFSET(struct obs_preflight_window, reason, 156);

static bool core_ready;
static bool graphics_effects_missing;

struct test_output_context {
	obs_output_t *output;
	atomic_uint_fast64_t total_bytes;
	atomic_int dropped_frames;
};

static const char *test_output_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "preflight collector test output";
}

static const char *test_encoder_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "preflight collector test encoder";
}

static void *test_encoder_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	return encoder;
}

static void test_encoder_destroy(void *unused)
{
	UNUSED_PARAMETER(unused);
}

static bool test_encoder_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
				bool *received_packet)
{
	static uint8_t packet_byte;
	UNUSED_PARAMETER(data);
	packet->data = &packet_byte;
	packet->size = 1;
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->type = OBS_ENCODER_VIDEO;
	packet->keyframe = true;
	*received_packet = true;
	return true;
}

static void *test_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct test_output_context *context = calloc(1, sizeof(*context));
	if (context) {
		context->output = output;
		atomic_init(&context->total_bytes, 0);
		atomic_init(&context->dropped_frames, 0);
	}
	return context;
}

static void test_output_destroy(void *data)
{
	free(data);
}

static bool test_output_start(void *data)
{
	struct test_output_context *context = data;
	if (!context)
		return false;
	obs_output_t *output = context->output;
	if (!obs_output_can_begin_data_capture(output, 0))
		return false;
	if (!obs_output_initialize_encoders(output, 0))
		return false;
	return obs_output_begin_data_capture(output, 0);
}

static void test_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct test_output_context *context = data;
	if (context)
		obs_output_end_data_capture(context->output);
}

static void test_output_packet(void *data, struct encoder_packet *packet)
{
	struct test_output_context *context = data;
	atomic_fetch_add_explicit(&context->total_bytes, (uint_fast64_t)packet->size, memory_order_relaxed);
}

static uint64_t test_output_total_bytes(void *data)
{
	struct test_output_context *context = data;
	return atomic_load_explicit(&context->total_bytes, memory_order_relaxed);
}

static int test_output_dropped_frames(void *data)
{
	struct test_output_context *context = data;
	return atomic_load_explicit(&context->dropped_frames, memory_order_relaxed);
}

static struct obs_encoder_info test_encoder_info = {
	.id = "preflight_collector_test_encoder",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = test_encoder_name,
	.create = test_encoder_create,
	.destroy = test_encoder_destroy,
	.encode = test_encoder_encode,
};

static struct obs_output_info test_output_info = {
	.id = "preflight_collector_test_output",
	.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED,
	.encoded_video_codecs = "h264",
	.get_name = test_output_name,
	.create = test_output_create,
	.destroy = test_output_destroy,
	.start = test_output_start,
	.stop = test_output_stop,
	.encoded_packet = test_output_packet,
	.get_total_bytes = test_output_total_bytes,
	.get_dropped_frames = test_output_dropped_frames,
};

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	graphics_effects_missing = false;
#if defined(__linux__)
	if (!getenv("DISPLAY")) {
		fprintf(stderr, "preflight collector test requires xvfb-run to provide DISPLAY\n");
		return -1;
	}
#endif
	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "preflight collector test: obs_startup failed\n");
		return -1;
	}
	if (!preflight_graphics_effects_available("preflight collector test")) {
		graphics_effects_missing = true;
		obs_shutdown();
		return 0;
	}

	struct obs_video_info ovi = {
		.graphics_module = GRAPHICS_MODULE_PATH,
		.fps_num = FPS,
		.fps_den = 1,
		.base_width = 64,
		.base_height = 64,
		.output_width = 64,
		.output_height = 64,
		.output_format = VIDEO_FORMAT_NV12,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "preflight collector test: obs_reset_video failed\n");
		obs_shutdown();
		return -1;
	}
	obs_register_encoder(&test_encoder_info);
	obs_register_output(&test_output_info);
	core_ready = true;
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	if (obs_initialized())
		obs_shutdown();
	return 0;
}

static void collector_samples_a_running_headless_output(void **state)
{
	UNUSED_PARAMETER(state);
	if (graphics_effects_missing)
		skip();
	assert_true(core_ready);

	obs_preflight_engine_t *engine = obs_preflight_engine_create_with_window(WINDOW_MS);
	obs_preflight_collector_t *collector = obs_preflight_collector_create();
	obs_encoder_t *encoder =
		obs_video_encoder_create("preflight_collector_test_encoder", "preflight encoder", NULL, NULL);
	obs_output_t *output = obs_output_create("preflight_collector_test_output", "preflight", NULL, NULL);
	assert_non_null(engine);
	assert_non_null(collector);
	assert_non_null(encoder);
	assert_non_null(output);
	obs_encoder_set_video(encoder, obs_get_video());
	obs_output_set_video_encoder(output, encoder);

	bool started = obs_output_start(output);
	if (!started) {
		fprintf(stderr, "test output failed to start: %s\n", obs_output_get_last_error(output));
		obs_output_release(output);
		obs_encoder_release(encoder);
		obs_preflight_collector_destroy(collector);
		obs_preflight_engine_destroy(engine);
		fail();
	}

	int collect_status = OBS_PREFLIGHT_OK;
	for (int i = 0; i < SAMPLE_COUNT; ++i) {
		os_sleep_ms(SAMPLE_INTERVAL_MS);
		collect_status = obs_preflight_collect_sample(collector, engine, output, obs_encoder_video(encoder));
		if (collect_status != OBS_PREFLIGHT_OK)
			break;
	}
	obs_output_force_stop(output);
	for (int i = 0; i < 200 && obs_output_active(output); ++i)
		os_sleep_ms(10);
	bool stopped = !obs_output_active(output);
	obs_output_release(output);
	obs_encoder_release(encoder);

	size_t window_count = 0;
	int count_status = obs_preflight_engine_get_window_count(engine, &window_count);
	struct obs_preflight_window window = {0};
	int window_status = count_status == OBS_PREFLIGHT_OK && window_count
				    ? obs_preflight_engine_get_window(engine, 0, &window)
				    : OBS_PREFLIGHT_ANALYSIS_ERROR;
	obs_preflight_collector_destroy(collector);
	obs_preflight_engine_destroy(engine);

	assert_int_equal(collect_status, OBS_PREFLIGHT_OK);
	assert_true(stopped);
	assert_int_equal(count_status, OBS_PREFLIGHT_OK);
	assert_true(window_count >= 2);
	assert_int_equal(window_status, OBS_PREFLIGHT_OK);
	assert_true(window.duration_ms >= SAMPLE_INTERVAL_MS);
	assert_true(window.output_bytes > 0);
	assert_true(window.output_frames > 0);
	assert_true(window.encoder_frames > 0);
	assert_true(window.frame_interval_ns > 0.0);
	assert_true(window.average_frame_time_ns >= 0.0);
	assert_true(isfinite(window.output_bitrate_kbps));
	assert_true(window.output_bitrate_kbps > 0.0);
	assert_true(isfinite(window.congestion_percent));
	assert_true(window.congestion_percent >= 0.0 && window.congestion_percent <= 100.0);
	assert_true(isfinite(window.cpu_percent));
	assert_true(window.cpu_percent >= 0.0 && window.cpu_percent <= 100.0);
	assert_true(window.connect_time_ms == -1 || window.connect_time_ms >= 0);
	assert_int_equal(window.output_frames_dropped, 0);
	assert_true(window.output_drop_percent >= 0.0 && window.output_drop_percent <= 100.0);
	assert_true(window.encoder_skip_percent >= 0.0 && window.encoder_skip_percent <= 100.0);
	assert_true(window.verdict >= OBS_PREFLIGHT_PASS && window.verdict <= OBS_PREFLIGHT_FAIL);
	assert_true(window.reason[0] != '\0');
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(collector_samples_a_running_headless_output),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}

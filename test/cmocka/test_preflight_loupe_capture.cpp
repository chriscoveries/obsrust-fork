#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <obs.h>
#include <obs-encoder.h>
#include <util/platform.h>

#include "preflight-loupe-capture.hpp"
#include "preflight-loupe-core.hpp"
#include "preflight_graphics_test_utils.h"

/* Keep cmocka last: its fail() macro collides with C++ standard headers. */
#include <cmocka.h>

namespace {

constexpr uint32_t FPS = 30;
constexpr uint32_t REALISTIC_WIDTH = 1920;
constexpr uint32_t REALISTIC_HEIGHT = 1080;
constexpr uint32_t RECONFIGURED_WIDTH = 128;
constexpr uint32_t RECONFIGURED_HEIGHT = 96;
/* The macOS virtual software renderer dropped about a third of 1080p frames;
 * give x264 enough source time to produce 60 decoded frames past its delay. */
constexpr uint32_t OUTPUT_DURATION_MS = 8000;
constexpr uint32_t RECONFIGURED_DURATION_MS = 4000;
constexpr uint64_t MIN_DECODED_FRAMES = FPS * 2;
constexpr uint64_t MIN_PAIR_RATE_PERCENT = 95;
constexpr uint64_t MAX_PAIR_DELTA_INTERVALS = 2;
constexpr uint64_t NANOSECONDS_PER_SECOND = 1000000000ULL;
constexpr uint8_t PATTERN_RED = 220;
constexpr uint8_t PATTERN_GREEN = 40;
constexpr uint8_t PATTERN_BLUE = 20;
constexpr uint8_t PATTERN_COLORS[][3] = {
	{PATTERN_RED, PATTERN_GREEN, PATTERN_BLUE},
	{20, 200, 40},
	{20, 40, 220},
	{210, 190, 30},
};
/* H.264 is lossy and OBS converts RGBA through 8-bit 4:2:0 NV12. Allow up to
 * 32 levels of average per-channel error in a flat crop interior. */
constexpr double MAX_PATTERN_CHANNEL_ERROR = 32.0;
constexpr char PATTERN_SOURCE_ID[] = "preflight_loupe_test_pattern";
constexpr char OUTPUT_ID[] = "preflight_loupe_test_output";
constexpr char AUDIO_ENCODER_ID[] = "preflight_loupe_test_audio_encoder";

struct PatternSource {
	uint32_t width;
	uint32_t height;
};

bool obs_up;
bool graphics_effects_missing;

const char *pattern_name(void *)
{
	return "Preflight loupe test pattern";
}

void *pattern_create(obs_data_t *settings, obs_source_t *)
{
	auto *data = new PatternSource;
	data->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	data->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	return data;
}

void pattern_destroy(void *data)
{
	delete static_cast<PatternSource *>(data);
}

uint32_t pattern_width(void *data)
{
	return static_cast<PatternSource *>(data)->width;
}

uint32_t pattern_height(void *data)
{
	return static_cast<PatternSource *>(data)->height;
}

void register_pattern_source()
{
	static const obs_source_info info = {
		.id = PATTERN_SOURCE_ID,
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO,
		.get_name = pattern_name,
		.create = pattern_create,
		.destroy = pattern_destroy,
		.get_width = pattern_width,
		.get_height = pattern_height,
	};
	obs_register_source(&info);
}

const char *output_name(void *)
{
	return "Preflight loupe test output";
}

void *output_create(obs_data_t *, obs_output_t *output)
{
	return output;
}

void output_destroy(void *) {}

bool output_start(void *data)
{
	auto *output = static_cast<obs_output_t *>(data);
	return obs_output_can_begin_data_capture(output, 0) && obs_output_initialize_encoders(output, 0) &&
	       obs_output_begin_data_capture(output, 0);
}

void output_stop(void *data, uint64_t)
{
	obs_output_end_data_capture(static_cast<obs_output_t *>(data));
}

void output_packet(void *, encoder_packet *) {}

const char *audio_encoder_name(void *)
{
	return "Preflight loupe test audio encoder";
}

void *audio_encoder_create(obs_data_t *, obs_encoder_t *)
{
	return new uint8_t{0};
}

void audio_encoder_destroy(void *data)
{
	delete static_cast<uint8_t *>(data);
}

bool audio_encoder_encode(void *, encoder_frame *frame, encoder_packet *packet, bool *received_packet)
{
	static uint8_t silence_packet = 0;
	packet->data = &silence_packet;
	packet->size = sizeof(silence_packet);
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->type = OBS_ENCODER_AUDIO;
	packet->keyframe = true;
	*received_packet = true;
	return true;
}

size_t audio_encoder_frame_size(void *)
{
	return 1024;
}

void register_test_output()
{
	static const obs_output_info info = {
		.id = OUTPUT_ID,
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
		.get_name = output_name,
		.create = output_create,
		.destroy = output_destroy,
		.start = output_start,
		.stop = output_stop,
		.encoded_packet = output_packet,
		.encoded_video_codecs = "h264",
		.encoded_audio_codecs = "aac",
	};
	obs_register_output(&info);
	static const obs_encoder_info audio_info = {
		.id = AUDIO_ENCODER_ID,
		.type = OBS_ENCODER_AUDIO,
		.codec = "aac",
		.get_name = audio_encoder_name,
		.create = audio_encoder_create,
		.destroy = audio_encoder_destroy,
		.encode = audio_encoder_encode,
		.get_frame_size = audio_encoder_frame_size,
	};
	obs_register_encoder(&audio_info);
}

void load_module(const char *path, const char *data_path)
{
	obs_module_t *module = nullptr;
	const int result = obs_open_module(&module, path, data_path);
	if (result != MODULE_SUCCESS || !obs_init_module(module)) {
		fprintf(stderr, "preflight loupe capture test: could not load encoder module %s\n", path);
		obs_up = false;
	}
}

int setup(void **state)
{
	(void)state;
	graphics_effects_missing = false;
#if defined(__linux__)
	if (!getenv("DISPLAY")) {
		fprintf(stderr, "preflight loupe capture test requires xvfb-run to provide DISPLAY\n");
		return -1;
	}
#endif
	if (!obs_startup("en-US", nullptr, nullptr)) {
		fprintf(stderr, "preflight loupe capture test: obs_startup failed\n");
		return -1;
	}
	obs_up = true;
	if (!preflight_graphics_effects_available("preflight loupe capture test")) {
		graphics_effects_missing = true;
		obs_shutdown();
		obs_up = false;
		return 0;
	}
	struct obs_video_info video_info = {
		.graphics_module = GRAPHICS_MODULE_PATH,
		.fps_num = FPS,
		.fps_den = 1,
		.base_width = REALISTIC_WIDTH,
		.base_height = REALISTIC_HEIGHT,
		.output_width = REALISTIC_WIDTH,
		.output_height = REALISTIC_HEIGHT,
		.output_format = VIDEO_FORMAT_NV12,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	if (obs_reset_video(&video_info) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "preflight loupe capture test: obs_reset_video failed\n");
		obs_shutdown();
		obs_up = false;
		return -1;
	}
	struct obs_audio_info audio_info = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&audio_info)) {
		fprintf(stderr, "preflight loupe capture test: obs_reset_audio failed\n");
		obs_shutdown();
		obs_up = false;
		return -1;
	}
	register_pattern_source();
	register_test_output();
#ifdef OBS_X264_MODULE_PATH
	load_module(OBS_X264_MODULE_PATH, OBS_X264_DATA_PATH);
#else
	return -1;
#endif
#ifdef OBS_VIDEOTOOLBOX_MODULE_PATH
	load_module(OBS_VIDEOTOOLBOX_MODULE_PATH, OBS_VIDEOTOOLBOX_DATA_PATH);
#endif
	return obs_up ? 0 : -1;
}

int teardown(void **state)
{
	(void)state;
	if (obs_initialized()) {
		obs_shutdown();
	}
	obs_up = false;
	return 0;
}

obs_source_t *create_pattern_source(uint32_t width, uint32_t height)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_source_t *source = obs_source_create(PATTERN_SOURCE_ID, "Preflight loupe pattern", settings, nullptr);
	obs_data_release(settings);
	return source;
}

void emit_pattern_frame(obs_source_t *source, uint32_t width, uint32_t height)
{
	obs_source_frame *frame = obs_source_frame_create(VIDEO_FORMAT_RGBA, width, height);
	frame->timestamp = os_gettime_ns();
	const uint32_t half_width = width / 2;
	const uint32_t half_height = height / 2;
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			uint8_t *pixel = frame->data[0] + static_cast<size_t>(y) * frame->linesize[0] + x * 4;
			if (x < half_width && y < half_height) {
				pixel[0] = PATTERN_RED;
				pixel[1] = PATTERN_GREEN;
				pixel[2] = PATTERN_BLUE;
			} else if (x >= half_width && y < half_height) {
				pixel[0] = 20;
				pixel[1] = 200;
				pixel[2] = 40;
			} else if (x < half_width) {
				pixel[0] = 20;
				pixel[1] = 40;
				pixel[2] = 220;
			} else {
				pixel[0] = 210;
				pixel[1] = 190;
				pixel[2] = 30;
			}
			pixel[3] = 255;
		}
	}
	obs_source_output_video(source, frame);
	obs_source_frame_destroy(frame);
}

void emit_pattern_for(obs_source_t *source, uint32_t width, uint32_t height, uint32_t duration_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		emit_pattern_frame(source, width, height);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000 / FPS));
	}
}

void stop_output(obs_output_t *output)
{
	if (obs_output_active(output)) {
		obs_output_force_stop(output);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (obs_output_active(output) && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	assert_false(obs_output_active(output));
}

bool wait_for_decoded_frames(PreflightLoupeCapture &capture, uint64_t target, uint32_t timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (capture.Stats().decoded_frames >= target) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return capture.Stats().decoded_frames >= target;
}

obs_encoder_t *create_x264_encoder(const char *id, const char *name, video_t *video)
{
	obs_data_t *settings = obs_data_create();
	obs_encoder_t *encoder = obs_video_encoder_create(id, name, settings, nullptr);
	obs_data_release(settings);
	if (encoder) {
		obs_encoder_set_video(encoder, video);
	}
	return encoder;
}

bool within_frame_intervals(uint64_t delta_ns, uint64_t intervals)
{
	obs_video_info video_info = {};
	if (!obs_get_video_info(&video_info) || video_info.fps_num == 0 || video_info.fps_den == 0) {
		return false;
	}
	/* Compare against the exact rational frame interval. obs_get_frame_interval_ns()
	 * truncates each interval before multiplying, which can reject the boundary. */
	const __uint128_t delta_scaled = static_cast<__uint128_t>(delta_ns) * video_info.fps_num;
	const __uint128_t limit_scaled =
		static_cast<__uint128_t>(intervals) * video_info.fps_den * NANOSECONDS_PER_SECOND;
	return delta_scaled <= limit_scaled;
}

bool pattern_crop_matches(const preflight::loupe::FramePair &pair)
{
	using namespace preflight::loupe;
	if (!pair.source || !pair.encoded) {
		return false;
	}

	const uint32_t source_width = pair.source->source_width ? pair.source->source_width : pair.source->width;
	const uint32_t source_height = pair.source->source_height ? pair.source->source_height : pair.source->height;
	if (pair.encoded->width != source_width || pair.encoded->height != source_height) {
		return false;
	}
	const auto source_region = make_crop_region(8, 8, 24, 24, source_width, source_height);
	if (!source_region) {
		return false;
	}
	const auto source_crop = crop_at_4x(*pair.source, *source_region);
	const auto encoded_crop = crop_at_4x(*pair.encoded, *source_region);
	if (!source_crop || !encoded_crop || source_crop->width != encoded_crop->width ||
	    source_crop->height != encoded_crop->height) {
		return false;
	}

	double source_mean[3] = {};
	double encoded_mean[3] = {};
	uint64_t pixels = 0;
	for (uint32_t y = 0; y < source_crop->height; ++y) {
		for (uint32_t x = 0; x < source_crop->width; ++x) {
			const size_t offset = static_cast<size_t>(y) * encoded_crop->stride + x * 4;
			for (size_t channel = 0; channel < 3; ++channel) {
				encoded_mean[channel] += encoded_crop->rgba[offset + channel];
			}
			const size_t source_offset = static_cast<size_t>(y) * source_crop->stride + x * 4;
			for (size_t channel = 0; channel < 3; ++channel) {
				source_mean[channel] += source_crop->rgba[source_offset + channel];
			}
			++pixels;
		}
	}
	if (!pixels) {
		return false;
	}
	for (size_t channel = 0; channel < 3; ++channel) {
		source_mean[channel] /= static_cast<double>(pixels);
		encoded_mean[channel] /= static_cast<double>(pixels);
	}
	bool source_is_pattern_color = false;
	/* OBS's graphics color-space conversion can shift a solid source tile slightly.
	 * This extra 16 levels only identifies which of the four known tiles was sampled. */
	for (const auto &color : PATTERN_COLORS) {
		if (std::abs(source_mean[0] - color[0]) <= MAX_PATTERN_CHANNEL_ERROR + 16.0 &&
		    std::abs(source_mean[1] - color[1]) <= MAX_PATTERN_CHANNEL_ERROR + 16.0 &&
		    std::abs(source_mean[2] - color[2]) <= MAX_PATTERN_CHANNEL_ERROR + 16.0) {
			source_is_pattern_color = true;
			break;
		}
	}
	const bool matches = source_is_pattern_color &&
			     std::abs(encoded_mean[0] - source_mean[0]) <= MAX_PATTERN_CHANNEL_ERROR &&
			     std::abs(encoded_mean[1] - source_mean[1]) <= MAX_PATTERN_CHANNEL_ERROR &&
			     std::abs(encoded_mean[2] - source_mean[2]) <= MAX_PATTERN_CHANNEL_ERROR;
	if (!matches) {
		fprintf(stderr, "loupe crop mean source RGB %.1f, %.1f, %.1f; encoded RGB %.1f, %.1f, %.1f\n",
			source_mean[0], source_mean[1], source_mean[2], encoded_mean[0], encoded_mean[1],
			encoded_mean[2]);
	}
	return matches;
}

bool capture_results_valid(PreflightLoupeCapture &capture, bool expect_packet_reorder, bool check_pattern_crop)
{
	const auto stats = capture.Stats();
	const bool high_pair_rate = stats.decoded_frames != 0 &&
				    static_cast<__uint128_t>(stats.paired_frames) * 100 >=
					    static_cast<__uint128_t>(stats.decoded_frames) * MIN_PAIR_RATE_PERCENT;
	if (stats.encoded_packets < MIN_DECODED_FRAMES || stats.decoded_frames < MIN_DECODED_FRAMES ||
	    stats.wrong_size_decoded_frames != 0 || !high_pair_rate ||
	    !within_frame_intervals(stats.max_capture_timestamp_delta_ns, MAX_PAIR_DELTA_INTERVALS) ||
	    (expect_packet_reorder && stats.reordered_packet_timestamps == 0) || stats.packet_queue_overflows != 0) {
		fprintf(stderr,
			"capture assertions failed: source=%llu packets=%llu decoded=%llu paired=%llu unpaired=%llu "
			"wrong_size=%llu reorder=%llu queue_overflows=%llu queue_bytes_peak=%llu pair_rate=%llu%% "
			"max_delta=%llu\n",
			static_cast<unsigned long long>(stats.source_frames),
			static_cast<unsigned long long>(stats.encoded_packets),
			static_cast<unsigned long long>(stats.decoded_frames),
			static_cast<unsigned long long>(stats.paired_frames),
			static_cast<unsigned long long>(stats.unpaired_decoded_frames),
			static_cast<unsigned long long>(stats.wrong_size_decoded_frames),
			static_cast<unsigned long long>(stats.reordered_packet_timestamps),
			static_cast<unsigned long long>(stats.packet_queue_overflows),
			static_cast<unsigned long long>(stats.maximum_queued_packet_bytes),
			static_cast<unsigned long long>(
				stats.decoded_frames ? stats.paired_frames * 100 / stats.decoded_frames : 0),
			static_cast<unsigned long long>(stats.max_capture_timestamp_delta_ns));
		return false;
	}
	const auto pair = capture.LatestPair();
	return pair && (!check_pattern_crop || pattern_crop_matches(*pair));
}

void cleanup_generation(PreflightLoupeCapture &capture, obs_source_t **source_slot, obs_output_t *output,
			obs_encoder_t **encoder_slot, obs_weak_encoder_t **weak_slot)
{
	capture.Stop();
	stop_output(output);
	obs_set_output_source(0, nullptr);
	if (*source_slot) {
		obs_source_release(*source_slot);
		*source_slot = nullptr;
	}
	obs_output_set_video_encoder(output, nullptr);
	if (*encoder_slot) {
		obs_encoder_release(*encoder_slot);
		*encoder_slot = nullptr;
	}
	if (*weak_slot) {
		obs_weak_encoder_release(*weak_slot);
		*weak_slot = nullptr;
	}
}

void configure_and_run(obs_source_t **source_slot, obs_output_t *output, const char *encoder_id, const char *name,
		       uint32_t width, uint32_t height, uint32_t duration_ms, bool expect_packet_reorder,
		       bool require_encoder, bool check_pattern_crop, PreflightLoupeCapture &capture,
		       obs_encoder_t **encoder_slot, obs_weak_encoder_t **weak_slot)
{
	obs_source_t *source = create_pattern_source(width, height);
	assert_non_null(source);
	*source_slot = source;
	obs_set_output_source(0, source);

	obs_encoder_t *encoder = create_x264_encoder(encoder_id, name, obs_get_video());
	if (!encoder && !require_encoder) {
		fprintf(stderr, "optional encoder %s could not initialize in this headless environment\n", encoder_id);
		obs_set_output_source(0, nullptr);
		obs_source_release(source);
		*source_slot = nullptr;
		return;
	}
	assert_non_null(encoder);
	*encoder_slot = encoder;
	*weak_slot = obs_encoder_get_weak_encoder(encoder);
	assert_non_null(*weak_slot);
	obs_output_set_video_encoder(output, encoder);

	if (!capture.Start(output, encoder)) {
		fprintf(stderr, "preflight loupe capture failed to start: %s\n", capture.Status().c_str());
		cleanup_generation(capture, source_slot, output, encoder_slot, weak_slot);
		fail_msg("production capture worker did not start");
	}
	if (width == REALISTIC_WIDTH) {
		const auto pattern_crop = preflight::loupe::make_crop_region(8, 8, 24, 24, width, height);
		assert_true(pattern_crop.has_value());
		assert_true(capture.SetSourceCrop(*pattern_crop));
	}
	if (!obs_output_start(output)) {
		fprintf(stderr, "preflight loupe test output failed to start: %s\n", obs_output_get_last_error(output));
		cleanup_generation(capture, source_slot, output, encoder_slot, weak_slot);
		fail_msg("real encoder output did not start");
	}
	emit_pattern_for(source, width, height, duration_ms);
	const bool enough_frames = wait_for_decoded_frames(capture, MIN_DECODED_FRAMES, 7000);
	const bool results_valid = enough_frames &&
				   capture_results_valid(capture, expect_packet_reorder, check_pattern_crop);
	if (!results_valid) {
		const auto stats = capture.Stats();
		fprintf(stderr,
			"loupe capture counters: source=%llu callbacks=%llu null_packet=%llu non_video=%llu missing_time=%llu "
			"zero_time=%llu packets=%llu decoded=%llu paired=%llu unpaired=%llu wrong_size=%llu reorders=%llu "
			"queue_overflows=%llu queue_bytes_peak=%llu "
			"max_delta_ns=%llu source_ts=%llu..%llu packet_ts=%llu..%llu status=%s\n",
			static_cast<unsigned long long>(stats.source_frames),
			static_cast<unsigned long long>(stats.packet_callback_calls),
			static_cast<unsigned long long>(stats.packet_callbacks_null_packet),
			static_cast<unsigned long long>(stats.packet_callbacks_non_video),
			static_cast<unsigned long long>(stats.packet_callbacks_without_timing),
			static_cast<unsigned long long>(stats.packet_callbacks_zero_timing),
			static_cast<unsigned long long>(stats.encoded_packets),
			static_cast<unsigned long long>(stats.decoded_frames),
			static_cast<unsigned long long>(stats.paired_frames),
			static_cast<unsigned long long>(stats.unpaired_decoded_frames),
			static_cast<unsigned long long>(stats.wrong_size_decoded_frames),
			static_cast<unsigned long long>(stats.reordered_packet_timestamps),
			static_cast<unsigned long long>(stats.packet_queue_overflows),
			static_cast<unsigned long long>(stats.maximum_queued_packet_bytes),
			static_cast<unsigned long long>(stats.max_capture_timestamp_delta_ns),
			static_cast<unsigned long long>(stats.first_source_timestamp_ns),
			static_cast<unsigned long long>(stats.last_source_timestamp_ns),
			static_cast<unsigned long long>(stats.first_packet_timestamp_ns),
			static_cast<unsigned long long>(stats.last_packet_timestamp_ns), capture.Status().c_str());
		cleanup_generation(capture, source_slot, output, encoder_slot, weak_slot);
		fail_msg("decoded dimensions, timestamp pairing, reorder, or crop assertion failed");
	}
}

void test_real_x264_packets_reach_the_loupe_worker(void **state)
{
	(void)state;
	if (graphics_effects_missing) {
		skip();
	}
	PreflightLoupeCapture capture;
	obs_output_t *output = obs_output_create(OUTPUT_ID, "preflight loupe output", nullptr, nullptr);
	assert_non_null(output);
	obs_data_t *audio_settings = obs_data_create();
	obs_encoder_t *audio_encoder =
		obs_audio_encoder_create(AUDIO_ENCODER_ID, "preflight loupe audio", audio_settings, 0, nullptr);
	obs_data_release(audio_settings);
	assert_non_null(audio_encoder);
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	obs_output_set_audio_encoder(output, audio_encoder, 0);
	obs_source_t *first_source = nullptr;
	obs_encoder_t *first_encoder = nullptr;
	obs_weak_encoder_t *first_weak = nullptr;

	configure_and_run(&first_source, output, "obs_x264", "preflight x264 1080p", REALISTIC_WIDTH, REALISTIC_HEIGHT,
			  OUTPUT_DURATION_MS, true, true, true, capture, &first_encoder, &first_weak);

	/* Stop capture while the output is still live, then stop and detach the old
	 * encoder. This exercises callback removal, worker join, and final ref release. */
	capture.Stop();
	assert_false(capture.Stats().worker_running);
	stop_output(output);
	obs_set_output_source(0, nullptr);
	obs_source_release(first_source);
	obs_output_set_video_encoder(output, nullptr);
	obs_encoder_release(first_encoder);
	obs_encoder_t *old_encoder_ref = obs_weak_encoder_get_encoder(first_weak);
	const bool old_encoder_released = old_encoder_ref == nullptr;
	if (old_encoder_ref) {
		obs_encoder_release(old_encoder_ref);
	}
	assert_true(old_encoder_released);
	obs_weak_encoder_release(first_weak);

	struct obs_video_info reconfigured = {
		.graphics_module = GRAPHICS_MODULE_PATH,
		.fps_num = FPS,
		.fps_den = 1,
		.base_width = RECONFIGURED_WIDTH,
		.base_height = RECONFIGURED_HEIGHT,
		.output_width = RECONFIGURED_WIDTH,
		.output_height = RECONFIGURED_HEIGHT,
		.output_format = VIDEO_FORMAT_NV12,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	assert_int_equal(obs_reset_video(&reconfigured), OBS_VIDEO_SUCCESS);

	obs_source_t *second_source = nullptr;
	obs_encoder_t *second_encoder = nullptr;
	obs_weak_encoder_t *second_weak = nullptr;
	configure_and_run(&second_source, output, "obs_x264", "preflight x264 reconfigured", RECONFIGURED_WIDTH,
			  RECONFIGURED_HEIGHT, RECONFIGURED_DURATION_MS, false, true, false, capture, &second_encoder,
			  &second_weak);
	capture.Stop();
	assert_false(capture.Stats().worker_running);
	stop_output(output);
	obs_set_output_source(0, nullptr);
	obs_source_release(second_source);
	obs_output_set_video_encoder(output, nullptr);
	obs_encoder_release(second_encoder);
	obs_encoder_t *reconfigured_encoder_ref = obs_weak_encoder_get_encoder(second_weak);
	const bool reconfigured_encoder_released = reconfigured_encoder_ref == nullptr;
	if (reconfigured_encoder_ref) {
		obs_encoder_release(reconfigured_encoder_ref);
	}
	assert_true(reconfigured_encoder_released);
	obs_weak_encoder_release(second_weak);
	obs_output_set_audio_encoder(output, nullptr, 0);
	obs_encoder_release(audio_encoder);
	obs_output_release(output);
}

#ifdef OBS_VIDEOTOOLBOX_MODULE_PATH
struct EncoderIdSearch {
	std::string id;
};

void test_registered_videotoolbox_h264_packets_reach_the_loupe_worker(void **state)
{
	(void)state;
	if (graphics_effects_missing) {
		skip();
	}
	std::string id;
	for (size_t index = 0;; ++index) {
		const char *candidate = nullptr;
		if (!obs_enum_encoder_types(index, &candidate)) {
			break;
		}
		const char *codec = obs_get_encoder_codec(candidate);
		if (codec && strcmp(codec, "h264") == 0 && strcmp(candidate, "obs_x264") != 0) {
			id = candidate;
			break;
		}
	}
	if (id.empty()) {
		fprintf(stderr,
			"VideoToolbox did not register a headless H.264 encoder; optional path not exercised\n");
		return;
	}
	fprintf(stderr, "exercising registered VideoToolbox H.264 encoder %s\n", id.c_str());

	PreflightLoupeCapture capture;
	obs_output_t *output = obs_output_create(OUTPUT_ID, "preflight loupe VideoToolbox output", nullptr, nullptr);
	assert_non_null(output);
	obs_data_t *audio_settings = obs_data_create();
	obs_encoder_t *audio_encoder = obs_audio_encoder_create(AUDIO_ENCODER_ID, "preflight loupe VideoToolbox audio",
								audio_settings, 0, nullptr);
	obs_data_release(audio_settings);
	assert_non_null(audio_encoder);
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	obs_output_set_audio_encoder(output, audio_encoder, 0);
	obs_source_t *source = nullptr;
	obs_encoder_t *encoder = nullptr;
	obs_weak_encoder_t *weak = nullptr;
	configure_and_run(&source, output, id.c_str(), "preflight VideoToolbox", RECONFIGURED_WIDTH,
			  RECONFIGURED_HEIGHT, RECONFIGURED_DURATION_MS, false, false, false, capture, &encoder, &weak);
	if (encoder) {
		capture.Stop();
		assert_false(capture.Stats().worker_running);
		stop_output(output);
		obs_set_output_source(0, nullptr);
		obs_source_release(source);
		obs_output_set_video_encoder(output, nullptr);
		obs_encoder_release(encoder);
		obs_encoder_t *live_ref = obs_weak_encoder_get_encoder(weak);
		const bool encoder_released = live_ref == nullptr;
		if (live_ref) {
			obs_encoder_release(live_ref);
		}
		assert_true(encoder_released);
		obs_weak_encoder_release(weak);
	}
	obs_output_set_audio_encoder(output, nullptr, 0);
	obs_encoder_release(audio_encoder);
	obs_output_release(output);
}
#endif

} // namespace

int main()
{
	const CMUnitTest tests[] = {
		cmocka_unit_test(test_real_x264_packets_reach_the_loupe_worker),
#ifdef OBS_VIDEOTOOLBOX_MODULE_PATH
		cmocka_unit_test(test_registered_videotoolbox_h264_packets_reach_the_loupe_worker),
#endif
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}

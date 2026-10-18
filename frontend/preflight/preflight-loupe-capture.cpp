#include "preflight-loupe-capture.hpp"

#include <obs-encoder.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t RAW_VIDEO_FRAME_RATE_DIVISOR = 4;
/* Keep several seconds of reordered 1080p packets for a slower software decoder,
 * while bounding queued packet memory independently of bitrate and resolution. */
constexpr size_t MAX_QUEUED_PACKETS = 512;
constexpr size_t MAX_QUEUED_PACKET_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_PACKET_BYTES = 8 * 1024 * 1024;
constexpr uint64_t MAX_RAW_FRAME_BYTES = 64 * 1024 * 1024;
constexpr uint32_t SOURCE_PREVIEW_MAX_DIMENSION = 640;
constexpr uint32_t SOURCE_DETAIL_MAX_DIMENSION = 256;
/* Sixty-four samples cover over eight seconds at the raw callback's 7.5 Hz
 * cadence, longer than x264's normal reorder/lookahead delay. The byte cap
 * keeps this history bounded on large canvases; at 1080p it retains 56 samples. */
constexpr size_t MAX_SOURCE_HISTORY_FRAMES = 64;
constexpr uint64_t MAX_SOURCE_HISTORY_BYTES = 64 * 1024 * 1024;

void preview_dimensions(uint32_t width, uint32_t height, uint32_t &preview_width, uint32_t &preview_height)
{
	if (width >= height) {
		preview_width = std::min(width, SOURCE_PREVIEW_MAX_DIMENSION);
		preview_height = std::max<uint32_t>(
			1, static_cast<uint32_t>(static_cast<uint64_t>(height) * preview_width / width));
	} else {
		preview_height = std::min(height, SOURCE_PREVIEW_MAX_DIMENSION);
		preview_width = std::max<uint32_t>(
			1, static_cast<uint32_t>(static_cast<uint64_t>(width) * preview_height / height));
	}
}

preflight::loupe::CropRegion center_detail_crop(uint32_t width, uint32_t height)
{
	const uint32_t crop_width = std::min<uint32_t>(64, width);
	const uint32_t crop_height = std::min<uint32_t>(36, height);
	return {width, height, (width - crop_width) / 2, (height - crop_height) / 2, crop_width, crop_height};
}

} // namespace

PreflightLoupeCapture::~PreflightLoupeCapture()
{
	Stop();
}

bool PreflightLoupeCapture::Start(obs_output_t *output_, obs_encoder_t *encoder)
{
	Stop();
	if (!output_ || !encoder) {
		std::lock_guard lock(mutex);
		status = "Loupe is waiting for a video encoder.";
		return false;
	}
	const char *codec_name = obs_encoder_get_codec(encoder);
	const uint32_t encoded_width = obs_encoder_get_width(encoder);
	const uint32_t encoded_height = obs_encoder_get_height(encoder);
	if (!codec_name || !*codec_name || encoded_width == 0 || encoded_height == 0) {
		std::lock_guard lock(mutex);
		status = "Loupe cannot read this encoder's codec and frame size.";
		return false;
	}
	if (encoded_width > UINT32_MAX / 4 ||
	    static_cast<uint64_t>(encoded_width) * encoded_height * 4 > MAX_RAW_FRAME_BYTES) {
		std::lock_guard lock(mutex);
		status = "Loupe cannot decode this encoded frame size safely.";
		return false;
	}

	obs_video_info video_info = {};
	if (!obs_get_video_info(&video_info) || video_info.output_width == 0 || video_info.output_height == 0 ||
	    video_info.fps_num == 0 || video_info.fps_den == 0 || video_info.output_width > UINT32_MAX / 4 ||
	    static_cast<uint64_t>(video_info.output_width) * video_info.output_height * 4 > MAX_RAW_FRAME_BYTES) {
		std::lock_guard lock(mutex);
		status = "Loupe cannot capture this source frame size safely.";
		return false;
	}

	decoder_config = {};
	decoder_config.codec_name = codec_name;
	decoder_config.width = encoded_width;
	decoder_config.height = encoded_height;
	uint8_t *extra_data = nullptr;
	size_t extra_data_size = 0;
	if (obs_encoder_get_extra_data(encoder, &extra_data, &extra_data_size) && extra_data && extra_data_size > 0) {
		decoder_config.extra_data.assign(extra_data, extra_data + extra_data_size);
	}

	output = output_;
	source_width = video_info.output_width;
	source_height = video_info.output_height;
	/* Frame timestamps are integer nanoseconds. Floor the exact rational bound so
	 * every accepted delta is no greater than two true frame intervals. */
	const uint64_t exact_tolerance_numerator = static_cast<uint64_t>(2) * video_info.fps_den * 1'000'000'000ULL;
	pair_tolerance_ns = std::max<uint64_t>(exact_tolerance_numerator / video_info.fps_num, 1);
	uint32_t preview_width = 0;
	uint32_t preview_height = 0;
	preview_dimensions(source_width, source_height, preview_width, preview_height);
	const uint64_t preview_bytes = static_cast<uint64_t>(preview_width) * preview_height * 4;
	const uint64_t max_detail_bytes =
		static_cast<uint64_t>(SOURCE_DETAIL_MAX_DIMENSION) * SOURCE_DETAIL_MAX_DIMENSION * 4;
	const uint64_t source_history_sample_bytes = preview_bytes + max_detail_bytes;
	const size_t source_history_frames = static_cast<size_t>(
		std::max<uint64_t>(1, std::min<uint64_t>(MAX_SOURCE_HISTORY_FRAMES,
							 MAX_SOURCE_HISTORY_BYTES / source_history_sample_bytes)));
	pairer.SetSourceCapacity(source_history_frames);
	{
		std::lock_guard lock(mutex);
		if (source_crop && source_crop->source_width == source_width &&
		    source_crop->source_height == source_height) {
			// Keep the user's selected region across a stop/start of the same video mode.
		} else if (source_crop) {
			const auto remapped = preflight::loupe::project_crop(*source_crop, source_width, source_height);
			if (remapped && remapped->width <= SOURCE_DETAIL_MAX_DIMENSION &&
			    remapped->height <= SOURCE_DETAIL_MAX_DIMENSION) {
				source_crop = preflight::loupe::CropRegion{source_width,    source_height,
									   remapped->left,  remapped->top,
									   remapped->width, remapped->height};
			} else {
				source_crop = center_detail_crop(source_width, source_height);
			}
		} else {
			source_crop = center_detail_crop(source_width, source_height);
		}
		packets.clear();
		queued_packet_bytes = 0;
		latest_pair.reset();
		pairer.Reset();
		accepting = false;
		stopping = false;
		decoder_ready = false;
		decoder_opened = false;
		await_keyframe = false;
		stats = {};
		last_packet_capture_timestamp_ns = 0;
		status = "Starting the encoded-frame decoder.";
	}
	worker = std::thread(&PreflightLoupeCapture::DecodeWorker, this);
	{
		std::unique_lock lock(mutex);
		condition.wait(lock, [this] { return decoder_ready; });
		if (!decoder_opened) {
			status = "This encoder's FFmpeg decoder is unavailable; the bench can still run.";
			lock.unlock();
			worker.join();
			output = nullptr;
			return false;
		}
		accepting = true;
		status = "Waiting for matching source and encoded frames.";
	}

	video_scale_info conversion = {};
	conversion.format = VIDEO_FORMAT_RGBA;
	conversion.width = video_info.output_width;
	conversion.height = video_info.output_height;
	conversion.range = VIDEO_RANGE_DEFAULT;
	conversion.colorspace = VIDEO_CS_DEFAULT;
	obs_add_raw_video_callback2(&conversion, RAW_VIDEO_FRAME_RATE_DIVISOR, RawVideoCallback, this);
	raw_callback_registered = true;
	obs_output_add_packet_callback(output, PacketCallback, this);
	packet_callback_registered = true;
	return true;
}

void PreflightLoupeCapture::Stop()
{
	if (packet_callback_registered && output) {
		obs_output_remove_packet_callback(output, PacketCallback, this);
		packet_callback_registered = false;
	}
	if (raw_callback_registered) {
		obs_remove_raw_video_callback(RawVideoCallback, this);
		raw_callback_registered = false;
	}
	if (worker.joinable()) {
		{
			std::lock_guard lock(mutex);
			accepting = false;
			stopping = true;
			packets.clear();
			queued_packet_bytes = 0;
		}
		condition.notify_all();
		worker.join();
	}
	{
		std::lock_guard lock(mutex);
		packets.clear();
		queued_packet_bytes = 0;
		latest_pair.reset();
		pairer.Reset();
		accepting = false;
		stopping = false;
		decoder_ready = false;
		decoder_opened = false;
		await_keyframe = false;
		if (status.find("decoder is unavailable") == std::string::npos) {
			status = "Loupe is waiting for the live bench.";
		}
	}
	output = nullptr;
	source_width = 0;
	source_height = 0;
}

std::shared_ptr<const preflight::loupe::FramePair> PreflightLoupeCapture::LatestPair() const
{
	std::lock_guard lock(mutex);
	return latest_pair;
}

PreflightLoupeCaptureStats PreflightLoupeCapture::Stats() const
{
	std::lock_guard lock(mutex);
	return stats;
}

std::string PreflightLoupeCapture::Status() const
{
	std::lock_guard lock(mutex);
	return status;
}

bool PreflightLoupeCapture::SetSourceCrop(const preflight::loupe::CropRegion &region)
{
	if (region.width == 0 || region.height == 0 || region.width > SOURCE_DETAIL_MAX_DIMENSION ||
	    region.height > SOURCE_DETAIL_MAX_DIMENSION ||
	    static_cast<uint64_t>(region.left) + region.width > region.source_width ||
	    static_cast<uint64_t>(region.top) + region.height > region.source_height) {
		return false;
	}
	std::lock_guard lock(mutex);
	if (source_width == 0 || source_height == 0 || region.source_width != source_width ||
	    region.source_height != source_height) {
		return false;
	}
	source_crop = region;
	return true;
}

void PreflightLoupeCapture::RawVideoCallback(void *param, video_data *frame)
{
	if (param && frame) {
		static_cast<PreflightLoupeCapture *>(param)->PushSourceFrame(*frame);
	}
}

void PreflightLoupeCapture::PacketCallback(obs_output_t *, encoder_packet *packet, encoder_packet_time *packet_time,
					   void *param)
{
	if (!param) {
		return;
	}
	auto *capture = static_cast<PreflightLoupeCapture *>(param);
	{
		std::lock_guard lock(capture->mutex);
		++capture->stats.packet_callback_calls;
		if (!packet) {
			++capture->stats.packet_callbacks_null_packet;
			return;
		}
		if (packet->type != OBS_ENCODER_VIDEO) {
			++capture->stats.packet_callbacks_non_video;
			return;
		}
		if (!packet_time) {
			++capture->stats.packet_callbacks_without_timing;
			return;
		}
		if (packet_time->cts == 0) {
			++capture->stats.packet_callbacks_zero_timing;
		}
	}
	if (packet_time) {
		capture->QueueEncodedPacket(*packet, *packet_time);
	}
}

void PreflightLoupeCapture::PushSourceFrame(const video_data &frame)
{
	if (!frame.data[0] || frame.timestamp == 0 || source_width == 0 || source_height == 0 ||
	    source_width > UINT32_MAX / 4 || frame.linesize[0] < source_width * 4) {
		return;
	}
	preflight::loupe::CropRegion detail_region;
	{
		std::lock_guard lock(mutex);
		if (!accepting || !source_crop) {
			return;
		}
		detail_region = *source_crop;
	}
	uint32_t preview_width = 0;
	uint32_t preview_height = 0;
	preview_dimensions(source_width, source_height, preview_width, preview_height);
	const uint64_t preview_bytes = static_cast<uint64_t>(preview_width) * preview_height * 4;
	const uint64_t detail_bytes = static_cast<uint64_t>(detail_region.width) * detail_region.height * 4;
	if (preview_bytes > MAX_RAW_FRAME_BYTES || detail_bytes > MAX_RAW_FRAME_BYTES ||
	    preview_bytes + detail_bytes > std::numeric_limits<size_t>::max()) {
		return;
	}
	auto source = std::make_shared<preflight::loupe::PixelFrame>();
	source->width = preview_width;
	source->height = preview_height;
	source->stride = source->width * 4;
	source->capture_timestamp_ns = frame.timestamp;
	source->source_width = source_width;
	source->source_height = source_height;
	source->rgba.resize(static_cast<size_t>(preview_bytes));
	for (uint32_t y = 0; y < preview_height; ++y) {
		const uint32_t source_y =
			static_cast<uint32_t>(static_cast<uint64_t>(y) * source_height / preview_height);
		for (uint32_t x = 0; x < preview_width; ++x) {
			const uint32_t source_x =
				static_cast<uint32_t>(static_cast<uint64_t>(x) * source_width / preview_width);
			const uint8_t *pixel =
				frame.data[0] + static_cast<size_t>(source_y) * frame.linesize[0] + source_x * 4;
			std::memcpy(source->rgba.data() + static_cast<size_t>(y) * source->stride + x * 4, pixel, 4);
		}
	}
	source->detail_region = {detail_region.left, detail_region.top, detail_region.width, detail_region.height};
	source->detail_stride = detail_region.width * 4;
	source->detail_rgba.resize(static_cast<size_t>(detail_bytes));
	for (uint32_t y = 0; y < detail_region.height; ++y) {
		const uint8_t *source_row = frame.data[0] +
					    static_cast<size_t>(detail_region.top + y) * frame.linesize[0] +
					    detail_region.left * 4;
		std::memcpy(source->detail_rgba.data() + static_cast<size_t>(y) * source->detail_stride, source_row,
			    source->detail_stride);
	}
	std::lock_guard lock(mutex);
	if (accepting) {
		if (stats.source_frames == 0) {
			stats.first_source_timestamp_ns = source->capture_timestamp_ns;
		}
		stats.last_source_timestamp_ns = source->capture_timestamp_ns;
		pairer.PushSource(std::move(source));
		++stats.source_frames;
	}
}

void PreflightLoupeCapture::QueueEncodedPacket(const encoder_packet &packet, const encoder_packet_time &packet_time)
{
	if (!packet.data || packet.size == 0 || packet.size > MAX_PACKET_BYTES || packet_time.cts == 0) {
		return;
	}
	preflight::loupe::EncodedVideoPacket copy;
	copy.bytes.assign(packet.data, packet.data + packet.size);
	copy.pts = packet.pts;
	copy.dts = packet.dts;
	copy.timebase_num = packet.timebase_num;
	copy.timebase_den = packet.timebase_den;
	copy.capture_timestamp_ns = packet_time.cts;
	copy.keyframe = packet.keyframe;
	{
		std::lock_guard lock(mutex);
		if (!accepting) {
			return;
		}
		if (last_packet_capture_timestamp_ns != 0 &&
		    copy.capture_timestamp_ns < last_packet_capture_timestamp_ns) {
			++stats.reordered_packet_timestamps;
		}
		if (stats.encoded_packets == 0) {
			stats.first_packet_timestamp_ns = copy.capture_timestamp_ns;
		}
		stats.last_packet_timestamp_ns = copy.capture_timestamp_ns;
		last_packet_capture_timestamp_ns = copy.capture_timestamp_ns;
		++stats.encoded_packets;
		const size_t packet_bytes = copy.bytes.size();
		if (packets.size() >= MAX_QUEUED_PACKETS ||
		    packet_bytes > MAX_QUEUED_PACKET_BYTES - queued_packet_bytes) {
			packets.clear();
			queued_packet_bytes = 0;
			++stats.packet_queue_overflows;
			await_keyframe = true;
		}
		if (await_keyframe && !copy.keyframe) {
			status = "Loupe is waiting for the encoder's next keyframe.";
			return;
		}
		const bool reset_decoder = await_keyframe;
		await_keyframe = false;
		packets.push_back({std::move(copy), reset_decoder});
		queued_packet_bytes += packet_bytes;
		stats.maximum_queued_packet_bytes =
			std::max<uint64_t>(stats.maximum_queued_packet_bytes, queued_packet_bytes);
	}
	condition.notify_one();
}

void PreflightLoupeCapture::DecodeWorker()
{
	{
		std::lock_guard lock(mutex);
		stats.worker_running = true;
	}
	const bool opened = decoder.Open(decoder_config);
	{
		std::lock_guard lock(mutex);
		decoder_ready = true;
		decoder_opened = opened;
		if (!opened) {
			stats.worker_running = false;
		}
	}
	condition.notify_all();
	if (!opened) {
		return;
	}

	while (true) {
		QueuedPacket queued;
		{
			std::unique_lock lock(mutex);
			condition.wait(lock, [this] { return stopping || !packets.empty(); });
			if (stopping) {
				break;
			}
			queued = std::move(packets.front());
			queued_packet_bytes -= queued.packet.bytes.size();
			packets.pop_front();
		}
		if (queued.reset_decoder) {
			decoder.Close();
			if (!decoder.Open(decoder_config)) {
				std::lock_guard lock(mutex);
				status = "Loupe decoder could not restart at a keyframe.";
				await_keyframe = true;
				continue;
			}
			std::lock_guard lock(mutex);
			pairer.Reset();
			latest_pair.reset();
		}

		std::vector<preflight::loupe::PixelFrame> decoded_frames;
		if (!decoder.Decode(queued.packet, decoded_frames)) {
			std::lock_guard lock(mutex);
			packets.clear();
			queued_packet_bytes = 0;
			await_keyframe = true;
			status = "Loupe decoder is resynchronizing at the next keyframe.";
			continue;
		}
		for (auto &decoded : decoded_frames) {
			auto encoded = std::make_shared<preflight::loupe::PixelFrame>(std::move(decoded));
			std::lock_guard lock(mutex);
			++stats.decoded_frames;
			if (encoded->width != decoder_config.width || encoded->height != decoder_config.height) {
				++stats.wrong_size_decoded_frames;
			}
			const auto pair = pairer.Match(encoded, pair_tolerance_ns);
			if (pair) {
				++stats.paired_frames;
				const uint64_t source_timestamp = pair->source->capture_timestamp_ns;
				const uint64_t encoded_timestamp = pair->encoded->capture_timestamp_ns;
				const uint64_t delta = source_timestamp > encoded_timestamp
							       ? source_timestamp - encoded_timestamp
							       : encoded_timestamp - source_timestamp;
				stats.max_capture_timestamp_delta_ns =
					std::max(stats.max_capture_timestamp_delta_ns, delta);
				latest_pair = std::make_shared<const preflight::loupe::FramePair>(*pair);
				status = "Showing matching source and encoded frames at 400%.";
			} else {
				++stats.unpaired_decoded_frames;
			}
		}
	}
	decoder.Close();
	{
		std::lock_guard lock(mutex);
		stats.worker_running = false;
	}
}

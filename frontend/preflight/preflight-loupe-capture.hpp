#pragma once

#include "preflight-loupe-core.hpp"

#include <obs.h>

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

struct PreflightLoupeCaptureStats {
	uint64_t source_frames = 0;
	uint64_t packet_callback_calls = 0;
	uint64_t packet_callbacks_null_packet = 0;
	uint64_t packet_callbacks_non_video = 0;
	uint64_t packet_callbacks_without_timing = 0;
	uint64_t packet_callbacks_zero_timing = 0;
	uint64_t encoded_packets = 0;
	uint64_t decoded_frames = 0;
	uint64_t paired_frames = 0;
	uint64_t unpaired_decoded_frames = 0;
	uint64_t wrong_size_decoded_frames = 0;
	uint64_t reordered_packet_timestamps = 0;
	uint64_t packet_queue_overflows = 0;
	uint64_t maximum_queued_packet_bytes = 0;
	uint64_t max_capture_timestamp_delta_ns = 0;
	uint64_t first_source_timestamp_ns = 0;
	uint64_t last_source_timestamp_ns = 0;
	uint64_t first_packet_timestamp_ns = 0;
	uint64_t last_packet_timestamp_ns = 0;
	bool worker_running = false;
};

class PreflightLoupeCapture {
	struct QueuedPacket {
		preflight::loupe::EncodedVideoPacket packet;
		bool reset_decoder = false;
	};

	preflight::loupe::FramePairer pairer{4};
	preflight::loupe::EncodedVideoDecoder decoder;
	preflight::loupe::DecoderConfig decoder_config;
	std::condition_variable condition;
	mutable std::mutex mutex;
	std::deque<QueuedPacket> packets;
	size_t queued_packet_bytes = 0;
	std::shared_ptr<const preflight::loupe::FramePair> latest_pair;
	std::thread worker;
	obs_output_t *output = nullptr;
	uint32_t source_width = 0;
	uint32_t source_height = 0;
	std::optional<preflight::loupe::CropRegion> source_crop;
	uint64_t pair_tolerance_ns = 0;
	bool accepting = false;
	bool stopping = false;
	bool decoder_ready = false;
	bool decoder_opened = false;
	bool await_keyframe = false;
	bool raw_callback_registered = false;
	bool packet_callback_registered = false;
	PreflightLoupeCaptureStats stats;
	uint64_t last_packet_capture_timestamp_ns = 0;
	std::string status;

	static void RawVideoCallback(void *param, video_data *frame);
	static void PacketCallback(obs_output_t *output, encoder_packet *packet, encoder_packet_time *packet_time,
				   void *param);
	void PushSourceFrame(const video_data &frame);
	void QueueEncodedPacket(const encoder_packet &packet, const encoder_packet_time &packet_time);
	void DecodeWorker();

public:
	PreflightLoupeCapture() = default;
	~PreflightLoupeCapture();
	PreflightLoupeCapture(const PreflightLoupeCapture &) = delete;
	PreflightLoupeCapture &operator=(const PreflightLoupeCapture &) = delete;

	bool Start(obs_output_t *output, obs_encoder_t *encoder);
	void Stop();
	std::shared_ptr<const preflight::loupe::FramePair> LatestPair() const;
	PreflightLoupeCaptureStats Stats() const;
	std::string Status() const;
	bool SetSourceCrop(const preflight::loupe::CropRegion &region);
};

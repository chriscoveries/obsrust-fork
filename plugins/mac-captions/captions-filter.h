#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <obs.h>

struct mac_captions_transcriber_ops {
	void *(*create)(void *factory_data);
	bool (*start)(void *session, uint32_t sample_rate,
		      void (*word)(void *param, const char *text, double start_sec, double end_sec), void *param);
	bool (*push)(void *session, const float *mono, uint32_t frames, uint64_t timestamp_ns);
	void (*stop)(void *session);
	void (*destroy)(void *session);
};

typedef void (*mac_captions_deliver_fn)(void *param, const uint8_t *cc_data);

struct mac_captions_core;

struct mac_captions_core *mac_captions_core_create(const struct mac_captions_transcriber_ops *ops, void *factory_data,
						   obs_source_t *source, uint32_t sample_rate, uint32_t channels,
						   bool enabled);
void mac_captions_core_set_enabled(struct mac_captions_core *core, bool enabled);
struct obs_audio_data *mac_captions_core_filter_audio(struct mac_captions_core *core, struct obs_audio_data *audio);
bool mac_captions_core_drain_one(struct mac_captions_core *core, mac_captions_deliver_fn deliver, void *param);
bool mac_captions_core_is_running(struct mac_captions_core *core);
void mac_captions_core_destroy(struct mac_captions_core *core);

extern const struct mac_captions_transcriber_ops mac_captions_speech_transcriber_ops;

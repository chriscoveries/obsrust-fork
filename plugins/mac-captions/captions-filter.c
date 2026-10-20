#include "captions-filter.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <media-io/audio-io.h>
#include <util/base.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include <callback/signal.h>

#include "captions-rollup.h"

#define MAX_QUEUED_AUDIO_SECONDS 3

struct audio_chunk {
	struct audio_chunk *next;
	uint32_t frames;
	uint64_t timestamp_ns;
	float samples[];
};

struct mac_captions_core {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t worker;
	bool worker_created;
	bool stopping;
	bool enabled;
	bool session_starting;
	bool session_running;
	bool start_failed;
	uint64_t enable_generation;
	uint32_t sample_rate;
	uint32_t channels;
	obs_source_t *source;
	signal_handler_t *source_signals;
	const struct mac_captions_transcriber_ops *ops;
	void *factory_data;
	struct audio_chunk *audio_head;
	struct audio_chunk *audio_tail;
	uint64_t queued_frames;
	struct captions_rollup rollup;
};

static void on_word(void *param, const char *text, double start_sec, double end_sec)
{
	struct mac_captions_core *core = param;
	(void)start_sec;
	(void)end_sec;
	pthread_mutex_lock(&core->mutex);
	bool enabled = core->enabled && !core->stopping;
	pthread_mutex_unlock(&core->mutex);
	if (enabled && !captions_rollup_word(&core->rollup, text))
		blog(LOG_WARNING, "[mac-captions] Could not encode a finalized word for CEA-608 roll-up captions");
}

static void free_audio_queue_locked(struct mac_captions_core *core)
{
	struct audio_chunk *chunk = core->audio_head;
	while (chunk) {
		struct audio_chunk *next = chunk->next;
		bfree(chunk);
		chunk = next;
	}
	core->audio_head = NULL;
	core->audio_tail = NULL;
	core->queued_frames = 0;
}

static void set_running(struct mac_captions_core *core, bool running)
{
	pthread_mutex_lock(&core->mutex);
	core->session_running = running;
	pthread_cond_broadcast(&core->cond);
	pthread_mutex_unlock(&core->mutex);
}

static void stop_session(struct mac_captions_core *core, void **session_ptr, bool started)
{
	void *session = *session_ptr;
	if (!session)
		return;
	if (started)
		core->ops->stop(session);
	core->ops->destroy(session);
	*session_ptr = NULL;
	set_running(core, false);
}

static void *worker_thread(void *param)
{
	struct mac_captions_core *core = param;
	void *session = NULL;
	bool started = false;
	uint64_t session_generation = 0;

	for (;;) {
		pthread_mutex_lock(&core->mutex);
		while (!core->stopping) {
			bool transition = session && (!core->enabled || session_generation != core->enable_generation);
			bool can_start = !session && core->enabled && !core->start_failed;
			bool has_audio = session && core->enabled && core->audio_head;
			if (transition || can_start || has_audio)
				break;
			pthread_cond_wait(&core->cond, &core->mutex);
		}
		if (core->stopping) {
			free_audio_queue_locked(core);
			pthread_mutex_unlock(&core->mutex);
			break;
		}

		if (session && (!core->enabled || session_generation != core->enable_generation)) {
			pthread_mutex_unlock(&core->mutex);
			stop_session(core, &session, started);
			started = false;
			continue;
		}

		if (!session) {
			uint64_t generation = core->enable_generation;
			core->session_starting = true;
			pthread_mutex_unlock(&core->mutex);
			session = core->ops->create(core->factory_data);
			started = session && core->ops->start(session, core->sample_rate, on_word, core);
			pthread_mutex_lock(&core->mutex);
			bool still_enabled = core->enabled && core->enable_generation == generation && !core->stopping;
			if (started && still_enabled) {
				session_generation = generation;
				core->start_failed = false;
				core->session_starting = false;
				core->session_running = true;
				pthread_cond_broadcast(&core->cond);
				pthread_mutex_unlock(&core->mutex);
				captions_rollup_reset(&core->rollup);
				continue;
			}
			if (!started && still_enabled)
				core->start_failed = true;
			core->session_running = started;
			pthread_mutex_unlock(&core->mutex);
			if (started)
				core->ops->stop(session);
			if (session)
				core->ops->destroy(session);
			session = NULL;
			started = false;
			pthread_mutex_lock(&core->mutex);
			core->session_starting = false;
			core->session_running = false;
			pthread_cond_broadcast(&core->cond);
			pthread_mutex_unlock(&core->mutex);
			continue;
		}

		struct audio_chunk *chunk = core->audio_head;
		if (!chunk) {
			pthread_cond_wait(&core->cond, &core->mutex);
			pthread_mutex_unlock(&core->mutex);
			continue;
		}
		core->audio_head = chunk->next;
		if (!core->audio_head)
			core->audio_tail = NULL;
		core->queued_frames -= chunk->frames;
		pthread_mutex_unlock(&core->mutex);

		bool ok = core->ops->push(session, chunk->samples, chunk->frames, chunk->timestamp_ns);
		bfree(chunk);
		if (!ok) {
			blog(LOG_WARNING,
			     "[mac-captions] Speech transcriber stopped accepting audio; toggling the filter off and on retries it");
			pthread_mutex_lock(&core->mutex);
			core->enabled = false;
			++core->enable_generation;
			free_audio_queue_locked(core);
			pthread_cond_broadcast(&core->cond);
			pthread_mutex_unlock(&core->mutex);
			captions_rollup_clear(&core->rollup);
		}
	}

	stop_session(core, &session, started);
	return NULL;
}

static void source_enable_signal(void *param, calldata_t *calldata)
{
	mac_captions_core_set_enabled(param, calldata_bool(calldata, "enabled"));
}

struct mac_captions_core *mac_captions_core_create(const struct mac_captions_transcriber_ops *ops, void *factory_data,
						   obs_source_t *source, uint32_t sample_rate, uint32_t channels,
						   bool enabled)
{
	if (!ops || !ops->create || !ops->start || !ops->push || !ops->stop || !ops->destroy || !sample_rate ||
	    !channels || channels > MAX_AV_PLANES)
		return NULL;

	struct mac_captions_core *core = bzalloc(sizeof(*core));
	if (!core)
		return NULL;
	if (pthread_mutex_init(&core->mutex, NULL) != 0) {
		bfree(core);
		return NULL;
	}
	if (pthread_cond_init(&core->cond, NULL) != 0) {
		pthread_mutex_destroy(&core->mutex);
		bfree(core);
		return NULL;
	}
	if (!captions_rollup_init(&core->rollup)) {
		pthread_cond_destroy(&core->cond);
		pthread_mutex_destroy(&core->mutex);
		bfree(core);
		return NULL;
	}
	core->ops = ops;
	core->factory_data = factory_data;
	core->source = source;
	core->sample_rate = sample_rate;
	core->channels = channels;
	core->enabled = enabled;
	if (source) {
		core->source_signals = obs_source_get_signal_handler(source);
		signal_handler_connect(core->source_signals, "enable", source_enable_signal, core);
	}
	if (pthread_create(&core->worker, NULL, worker_thread, core) != 0) {
		if (core->source_signals)
			signal_handler_disconnect(core->source_signals, "enable", source_enable_signal, core);
		captions_rollup_destroy(&core->rollup);
		pthread_cond_destroy(&core->cond);
		pthread_mutex_destroy(&core->mutex);
		bfree(core);
		return NULL;
	}
	core->worker_created = true;
	return core;
}

void mac_captions_core_set_enabled(struct mac_captions_core *core, bool enabled)
{
	pthread_mutex_lock(&core->mutex);
	if (core->enabled != enabled) {
		core->enabled = enabled;
		++core->enable_generation;
		if (!enabled) {
			core->start_failed = false;
			free_audio_queue_locked(core);
		}
		pthread_cond_broadcast(&core->cond);
	}
	pthread_mutex_unlock(&core->mutex);
	if (!enabled) {
		captions_rollup_clear(&core->rollup);
		pthread_mutex_lock(&core->mutex);
		while (core->session_starting || core->session_running)
			pthread_cond_wait(&core->cond, &core->mutex);
		pthread_mutex_unlock(&core->mutex);
	}
}

struct obs_audio_data *mac_captions_core_filter_audio(struct mac_captions_core *core, struct obs_audio_data *audio)
{
	if (!audio || !audio->frames)
		return audio;
	pthread_mutex_lock(&core->mutex);
	bool enabled = core->enabled && !core->stopping;
	pthread_mutex_unlock(&core->mutex);
	if (!enabled)
		return audio;

	for (uint32_t channel = 0; channel < core->channels; ++channel) {
		if (!audio->data[channel])
			return audio;
	}
	struct audio_chunk *chunk = bzalloc(sizeof(*chunk) + (size_t)audio->frames * sizeof(float));
	if (!chunk)
		return audio;
	chunk->frames = audio->frames;
	chunk->timestamp_ns = audio->timestamp;
	for (uint32_t frame = 0; frame < audio->frames; ++frame) {
		float sample = 0.0f;
		for (uint32_t channel = 0; channel < core->channels; ++channel)
			sample += ((const float *)audio->data[channel])[frame];
		chunk->samples[frame] = sample / (float)core->channels;
	}

	pthread_mutex_lock(&core->mutex);
	uint64_t queue_limit = (uint64_t)core->sample_rate * MAX_QUEUED_AUDIO_SECONDS;
	if (!core->enabled || core->stopping || core->queued_frames + chunk->frames > queue_limit) {
		pthread_mutex_unlock(&core->mutex);
		bfree(chunk);
		return audio;
	}
	if (core->audio_tail)
		core->audio_tail->next = chunk;
	else
		core->audio_head = chunk;
	core->audio_tail = chunk;
	core->queued_frames += chunk->frames;
	pthread_cond_signal(&core->cond);
	pthread_mutex_unlock(&core->mutex);
	return audio;
}

bool mac_captions_core_drain_one(struct mac_captions_core *core, mac_captions_deliver_fn deliver, void *param)
{
	if (!deliver)
		return false;
	pthread_mutex_lock(&core->mutex);
	bool enabled = core->enabled && !core->stopping;
	pthread_mutex_unlock(&core->mutex);
	if (!enabled)
		return false;

	uint16_t pair;
	if (!captions_rollup_pop(&core->rollup, &pair))
		return false;
	uint8_t cc_data[3] = {0xFC, (uint8_t)(pair >> 8), (uint8_t)pair};
	deliver(param, cc_data);
	return true;
}

bool mac_captions_core_is_running(struct mac_captions_core *core)
{
	pthread_mutex_lock(&core->mutex);
	bool running = core->session_running;
	pthread_mutex_unlock(&core->mutex);
	return running;
}

void mac_captions_core_destroy(struct mac_captions_core *core)
{
	if (!core)
		return;
	if (core->source_signals)
		signal_handler_disconnect(core->source_signals, "enable", source_enable_signal, core);
	pthread_mutex_lock(&core->mutex);
	core->stopping = true;
	core->enabled = false;
	++core->enable_generation;
	free_audio_queue_locked(core);
	pthread_cond_broadcast(&core->cond);
	pthread_mutex_unlock(&core->mutex);
	if (core->worker_created)
		pthread_join(core->worker, NULL);
	captions_rollup_destroy(&core->rollup);
	pthread_cond_destroy(&core->cond);
	pthread_mutex_destroy(&core->mutex);
	bfree(core);
}

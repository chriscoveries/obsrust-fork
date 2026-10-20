#include <obs-module.h>

#include <media-io/audio-io.h>

#include "captions-filter.h"
#include "speech-transcriber.h"

struct mac_captions_filter {
	struct mac_captions_core *core;
};

static const char *filter_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("LiveCaptionsName");
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct obs_audio_info audio_info;
	if (!obs_get_audio_info(&audio_info))
		return NULL;

	uint32_t channels = get_audio_channels(audio_info.speakers);
	if (!channels)
		return NULL;

	struct mac_captions_filter *filter = bzalloc(sizeof(*filter));
	filter->core = mac_captions_core_create(&mac_captions_speech_transcriber_ops, NULL, source,
						audio_info.samples_per_sec, channels, obs_source_enabled(source));
	if (!filter->core) {
		bfree(filter);
		return NULL;
	}
	return filter;
}

static void filter_destroy(void *data)
{
	struct mac_captions_filter *filter = data;
	mac_captions_core_destroy(filter->core);
	bfree(filter);
}

static struct obs_audio_data *filter_audio(void *data, struct obs_audio_data *audio)
{
	struct mac_captions_filter *filter = data;
	return mac_captions_core_filter_audio(filter->core, audio);
}

struct caption_delivery {
	const uint8_t *data;
};

static bool deliver_to_active_output(void *param, obs_output_t *output)
{
	struct caption_delivery *delivery = param;
	if (!obs_output_active(output))
		return true;

	struct obs_source_cea_708 packet = {.data = delivery->data, .packets = 1};
	obs_output_caption(output, &packet);
	return true;
}

static void deliver_caption(void *param, const uint8_t *cc_data)
{
	UNUSED_PARAMETER(param);
	struct caption_delivery delivery = {.data = cc_data};
	obs_enum_outputs(deliver_to_active_output, &delivery);
}

static void filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct mac_captions_filter *filter = data;
	mac_captions_core_drain_one(filter->core, deliver_caption, NULL);
}

struct obs_source_info mac_live_captions_filter = {
	.id = "mac_live_captions_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.video_tick = filter_video_tick,
	.filter_audio = filter_audio,
};

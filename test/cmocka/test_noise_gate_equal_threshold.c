/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Headless production noise-gate callback at equal and normal thresholds. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../plugins/obs-filters/noise-gate-filter.c.h"

const char *obs_module_text(const char *text)
{
	return text;
}

static unsigned checks;
static unsigned failures;

static void check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		if (failures < 8)
			fprintf(stderr, "%s\n", message);
		++failures;
	}
}

static bool same_float(float a, float b)
{
	return memcmp(&a, &b, sizeof(a)) == 0;
}

static void reference_audio(struct noise_gate_data *ng, struct obs_audio_data *audio)
{
	float **adata = (float **)audio->data;
	const float close_threshold = ng->close_threshold;
	const float open_threshold = ng->open_threshold;
	const float sample_rate_i = ng->sample_rate_i;
	const float release_rate = ng->release_rate;
	const float attack_rate = ng->attack_rate;
	const float decay_rate = ng->decay_rate;
	const float hold_time = ng->hold_time;

	for (size_t i = 0; i < audio->frames; ++i) {
		float cur_level = fabsf(adata[0][i]);
		for (size_t j = 0; j < ng->channels; ++j)
			cur_level = fmaxf(cur_level, fabsf(adata[j][i]));
		if (cur_level > open_threshold && !ng->is_open)
			ng->is_open = true;
		if (ng->level < close_threshold && ng->is_open) {
			ng->held_time = 0.0f;
			ng->is_open = false;
		}
		ng->level = fmaxf(ng->level, cur_level) - decay_rate;
		if (ng->is_open) {
			ng->attenuation = fminf(1.0f, ng->attenuation + attack_rate);
		} else {
			ng->held_time += sample_rate_i;
			if (ng->held_time > hold_time)
				ng->attenuation = fmaxf(0.0f, ng->attenuation - release_rate);
		}
		for (size_t j = 0; j < ng->channels; ++j)
			adata[j][i] *= ng->attenuation;
	}
}

static struct noise_gate_data initial(float sample_rate, size_t channels, bool equal)
{
	const float open = db_to_mul(-26.0f);
	const float close = equal ? open : db_to_mul(-32.0f);
	struct noise_gate_data state = {0};
	state.sample_rate_i = 1.0f / sample_rate;
	state.channels = channels;
	state.open_threshold = open;
	state.close_threshold = close;
	state.decay_rate = (open - close) / (sample_rate / 75.0f);
	state.attack_rate = 1.0f / (0.025f * sample_rate);
	state.release_rate = 1.0f / (0.15f * sample_rate);
	state.hold_time = 0.005f;
	return state;
}

static void equal_threshold_closes_after_silence(void)
{
	const float rates[] = {8000.0f, 44100.0f, 48000.0f, 96000.0f, 192000.0f};
	const size_t channels[] = {1, 2, 6, 8};
	for (size_t rate = 0; rate < sizeof(rates) / sizeof(rates[0]); ++rate) {
		for (size_t count = 0; count < sizeof(channels) / sizeof(channels[0]); ++count) {
			struct noise_gate_data state = initial(rates[rate], channels[count], true);
			float samples[MAX_AUDIO_CHANNELS][1024] = {{0}};
			struct obs_audio_data audio = {0};
			audio.frames = 1024;
			for (size_t c = 0; c < state.channels; ++c) {
				audio.data[c] = (uint8_t *)samples[c];
				for (size_t i = 0; i < 128; ++i)
					samples[c][i] = 0.5f;
			}
			check(state.decay_rate == 0.0f, "equal_threshold_zero_decay: setup mismatch");
			/* End the opening block before sending silence. The fixed detector
			 * is allowed to close during silence; that is the regression. */
			audio.frames = 128;
			noise_gate_filter_audio(&state, &audio);
			check(state.is_open, "equal_threshold_opens_on_signal: gate never opened");
			for (size_t c = 0; c < state.channels; ++c)
				memset(samples[c], 0, sizeof(samples[c]));
			audio.frames = 1024;
			for (int block = 0; block < 64; ++block)
				noise_gate_filter_audio(&state, &audio);
			check(!state.is_open, "equal_threshold_closes_after_silence: gate remains open");
			check(state.attenuation == 0.0f,
			      "equal_threshold_releases_after_silence: output remains unattenuated");
		}
	}
}

static void normal_hysteresis_is_bit_exact(void)
{
	for (size_t channels = 1; channels <= 8; channels *= 2) {
		struct noise_gate_data a = initial(48000.0f, channels, false);
		struct noise_gate_data b = a;
		float actual[MAX_AUDIO_CHANNELS][1024] = {{0}};
		float expected[MAX_AUDIO_CHANNELS][1024] = {{0}};
		struct obs_audio_data aa = {0};
		struct obs_audio_data bb = {0};
		for (size_t c = 0; c < channels; ++c) {
			aa.data[c] = (uint8_t *)actual[c];
			bb.data[c] = (uint8_t *)expected[c];
		}
		for (size_t block = 0; block < 12; ++block) {
			aa.frames = bb.frames = block % 2 == 0 ? 1024 : 17;
			for (size_t c = 0; c < channels; ++c) {
				for (size_t i = 0; i < aa.frames; ++i) {
					float v = ((i + block * 7 + c * 3) % 91 < 22) ? 0.41f : 0.0f;
					if ((i + c) & 1)
						v = -v;
					actual[c][i] = expected[c][i] = v;
				}
			}
			noise_gate_filter_audio(&a, &aa);
			reference_audio(&b, &bb);
			for (size_t c = 0; c < channels; ++c)
				for (size_t i = 0; i < aa.frames; ++i)
					check(same_float(actual[c][i], expected[c][i]),
					      "normal_hysteresis_is_bit_exact: output diverged");
			check(a.is_open == b.is_open && same_float(a.level, b.level) &&
				      same_float(a.attenuation, b.attenuation) && same_float(a.held_time, b.held_time),
			      "normal_hysteresis_is_bit_exact: carried state diverged");
		}
	}
}

int main(void)
{
	equal_threshold_closes_after_silence();
	normal_hysteresis_is_bit_exact();
	printf("noise-gate equality: checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}

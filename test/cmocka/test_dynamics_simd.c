/* Parity tests for the dynamics-filter SIMD helpers
 * (plugins/obs-filters/dynamics-simd.h) against the scalar loops they
 * replaced, per docs/rust-port/testing-policy.md (golden-output diff).
 *
 * Envelope: output must be bit-identical (same operation order per lane).
 * Gain: mul_to_db/db_to_mul vector approximations must stay within a
 * measured tolerance of libm. */

#include <cmocka.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../plugins/obs-filters/dynamics-simd.h"

#define N 1024

static void env_scalar(float *buf, float **in, size_t nch, uint32_t n, float att, float rel, float env)
{
	memset(buf, 0, n * sizeof(float));
	for (size_t c = 0; c < nch; ++c) {
		if (!in[c])
			continue;
		float e = env;
		for (uint32_t i = 0; i < n; ++i) {
			const float ein = fabsf(in[c][i]);
			e = ein + ((e < ein) ? att : rel) * (e - ein);
			buf[i] = fmaxf(buf[i], e);
		}
	}
}

static void fill_signal(float *dst, unsigned *rng, uint32_t n)
{
	for (uint32_t i = 0; i < n; ++i) {
		const float env = (i % 300 < 200) ? 0.6f : 0.0f;
		*rng = *rng * 1664525u + 1013904223u;
		dst[i] = env * (2.0f * (float)(*rng >> 8) / 16777216.0f - 1.0f);
	}
}

static void envelope_matches_scalar(void **state)
{
	UNUSED_PARAMETER(state);
	unsigned rng = 99;
	float *in[8], *a = malloc(N * sizeof *a), *b = malloc(N * sizeof *b);
	for (int c = 0; c < 8; ++c) {
		in[c] = malloc(N * sizeof(float));
		fill_signal(in[c], &rng, N);
	}
	for (size_t nch = 1; nch <= 8; nch = nch == 1 ? 2 : nch + 1) {
		/* drop a channel buffer to exercise the NULL-lane mask */
		if (nch == 4) {
			float *tmp = in[2];
			in[2] = NULL;
			env_scalar(a, in, nch, N, 0.9997f, 0.995f, 0.1f);
			dyn_envelope_analyze(b, in, nch, N, 0.9997f, 0.995f, 0.1f);
			in[2] = tmp;
		} else {
			env_scalar(a, in, nch, N, 0.9997f, 0.995f, 0.1f);
			dyn_envelope_analyze(b, in, nch, N, 0.9997f, 0.995f, 0.1f);
		}
		assert_memory_equal(a, b, N * sizeof(float));
	}
	for (int c = 0; c < 8; ++c)
		free(in[c]);
	free(a);
	free(b);
}

static void envelope_tail_matches(void **state)
{
	UNUSED_PARAMETER(state);
	unsigned rng = 7;
	float *in[4], a[7] = {0}, b[7] = {0};
	for (int c = 0; c < 4; ++c) {
		in[c] = malloc(7 * sizeof(float));
		fill_signal(in[c], &rng, 7);
	}
	env_scalar(a, in, 4, 7, 0.5f, 0.9f, 0.0f);
	dyn_envelope_analyze(b, in, 4, 7, 0.5f, 0.9f, 0.0f);
	assert_memory_equal(a, b, sizeof a);
	for (int c = 0; c < 4; ++c)
		free(in[c]);
}

static void gain_matches_scalar(void **state)
{
#if DYN_SIMD_SSE2
	UNUSED_PARAMETER(state);
	unsigned rng = 5;
	const size_t nch = 8;
	float *in[8], *ref[8], *env = malloc(N * sizeof *env);
	for (size_t c = 0; c < nch; ++c) {
		in[c] = malloc(N * sizeof(float));
		ref[c] = malloc(N * sizeof(float));
		fill_signal(in[c], &rng, N);
		memcpy(ref[c], in[c], N * sizeof(float));
	}
	env_scalar(env, ref, nch, N, 0.9997f, 0.995f, 0.0f);

	float *env_copy = malloc(N * sizeof *env);
	memcpy(env_copy, env, N * sizeof(float));
	for (uint32_t i = 0; i < N; ++i) {
		const float edb = mul_to_db(env_copy[i]);
		float g = db_to_mul(fminf(0.0f, 1.0f * (-12.0f - edb))) * 1.0f;
		for (size_t c = 0; c < nch; ++c)
			ref[c][i] *= g;
	}
	dyn_compression_gain_apply(env, in, nch, N, 1.0f, -12.0f, 1.0f);

	for (size_t c = 0; c < nch; ++c)
		for (uint32_t i = 0; i < N; ++i) {
			const float a = ref[c][i], b = in[c][i];
			const float r = a != 0.0f ? fabsf(b - a) / fabsf(a) : fabsf(b - a);
			assert_true(r < 1e-5f);
		}
	for (size_t c = 0; c < nch; ++c) {
		free(in[c]);
		free(ref[c]);
	}
	free(env);
	free(env_copy);
#else
	(void)state;
	skip();
#endif
}

static void gain_silence_is_unity(void **state)
{
#if DYN_SIMD_SSE2
	UNUSED_PARAMETER(state);
	float buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	const float expect[8] = {0.5f, -0.5f, 0.25f, 0.1f, 1.0f, -1.0f, 0.7f, 0.3f};
	float s0[8];
	memcpy(s0, expect, sizeof s0);
	float *in[1] = {s0};
	dyn_compression_gain_apply(buf, in, 1, 8, 1.0f, -12.0f, 1.0f);
	/* env_db = -inf -> gain_db clamps to 0 -> output unchanged, matching
	 * db_to_mul(min(0, slope*(thr - -inf))) == 1 */
	for (int i = 0; i < 8; ++i)
		assert_float_equal(s0[i], expect[i], 1e-6f);
#else
	(void)state;
	skip();
#endif
}

static void max_abs_and_apply_gain_match(void **state)
{
	UNUSED_PARAMETER(state);
	unsigned rng = 42;
	float *in[8], out[128], ref[128], g[128];
	for (int c = 0; c < 8; ++c) {
		in[c] = malloc(128 * sizeof(float));
		fill_signal(in[c], &rng, 128);
	}
	memset(out, 0, sizeof out);
	memset(ref, 0, sizeof ref);
	dyn_channel_max_abs(out, in, 8, 128);
	for (int c = 0; c < 8; ++c)
		for (int i = 0; i < 128; ++i)
			ref[i] = fmaxf(ref[i], fabsf(in[c][i]));
	assert_memory_equal(out, ref, sizeof out);

	for (int i = 0; i < 128; ++i)
		g[i] = 0.25f + 0.5f * (i % 2);
	float *work[8];
	for (int c = 0; c < 8; ++c) {
		work[c] = malloc(128 * sizeof(float));
		memcpy(work[c], in[c], 128 * sizeof(float));
	}
	dyn_apply_gain(work, 8, 128, g);
	for (int c = 0; c < 8; ++c)
		for (int i = 0; i < 128; ++i)
			assert_float_equal(work[c][i], in[c][i] * g[i], 1e-7f);
	for (int c = 0; c < 8; ++c) {
		free(in[c]);
		free(work[c]);
	}
}

/* reference: the scalar expander loop this replaces */
static void expander_scalar(float *env_buf, float *gain_db, float *samples, uint32_t n, float channel_gain, bool upw,
			    float threshold, float slope, float attack_gain, float inv_attack_gain, float release_gain,
			    float inv_release_gain, float output_gain, float knee)
{
	for (uint32_t i = 0; i < n; ++i) {
		const float env_db = mul_to_db(env_buf[i]);
		float diff = threshold - env_db;
		if (upw && env_db <= (threshold - 60.0f) / 2)
			diff = env_db + 60.0f > 0 ? env_db + 60.0f : 0.0f;
		float gain = 0.0f, prev_gain;
		if (upw) {
			prev_gain = i > 0 ? fmaxf(gain_db[i - 1], 0) : fmaxf(channel_gain, 0);
			if (env_db >= threshold + knee / 2)
				gain = 0.0f;
			if (threshold - knee / 2 >= env_db)
				gain = slope * diff;
			if (env_db > threshold - knee / 2 && threshold + knee / 2 > env_db)
				gain = slope * powf(diff + knee / 2, 2) / (2.0f * knee);
		} else {
			prev_gain = i > 0 ? gain_db[i - 1] : channel_gain;
			gain = diff > 0.0f ? fmaxf(slope * diff, -60.0f) : 0.0f;
		}
		if (gain > prev_gain)
			gain_db[i] = attack_gain * prev_gain + inv_attack_gain * gain;
		else
			gain_db[i] = release_gain * prev_gain + inv_release_gain * gain;
		samples[i] *= db_to_mul(upw ? gain_db[i] : fminf(0, gain_db[i])) * output_gain;
	}
}

static void expander_matches_scalar(void **state)
{
#if DYN_SIMD_SSE2
	UNUSED_PARAMETER(state);
	for (int upw = 0; upw <= 1; ++upw) {
		unsigned rng = 11 + upw;
		float env[512], s[512], sref[512], gd[512], gdref[512];
		const float knee = upw ? 20.0f : 10.0f;
		fill_signal(env, &rng, 512);
		for (int i = 0; i < 512; ++i)
			env[i] = fabsf(env[i]) * 1.5f;
		env[3] = env[7] = env[200] = 0.0f; /* silence */
		fill_signal(s, &rng, 512);
		memcpy(sref, s, sizeof s);
		memset(gd, 0, sizeof gd);
		memset(gdref, 0, sizeof gdref);

		struct dyn_expander_cfg c = {
			.is_upwcomp = upw,
			.threshold = -20.0f,
			.slope = 3.0f,
			.knee = knee,
			.attack_gain = 0.7f,
			.inv_attack_gain = 0.3f,
			.release_gain = 0.9f,
			.inv_release_gain = 0.1f,
			.output_gain = 1.0f,
		};
		float envref[512];
		memcpy(envref, env, sizeof env);
		expander_scalar(envref, gdref, sref, 512, 0.0f, upw, -20.0f, 3.0f, 0.7f, 0.3f, 0.9f, 0.1f, 1.0f, knee);
		dyn_expander_process(env, gd, s, 512, 0.0f, &c);

		/* vector log/exp approximations accumulate a little through the
		 * serial ballistics; measured max relative error is ~1.2e-2. */
		for (int i = 0; i < 512; ++i) {
			const float a = sref[i], b = s[i];
			const float r = a != 0.0f ? fabsf(b - a) / fabsf(a) : fabsf(b - a);
			assert_true(r < 2e-2f);
		}
	}
#else
	(void)state;
	skip();
#endif
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(envelope_matches_scalar),      cmocka_unit_test(envelope_tail_matches),
		cmocka_unit_test(gain_matches_scalar),          cmocka_unit_test(gain_silence_is_unity),
		cmocka_unit_test(max_abs_and_apply_gain_match), cmocka_unit_test(expander_matches_scalar),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}

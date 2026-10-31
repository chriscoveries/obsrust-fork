/******************************************************************************
    Copyright (C) 2026 by Hugh Bailey ("Jim") <jim@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/* Shared SIMD helpers for the dynamics filters (limiter, compressor,
 * expander, noise gate). Two data-parallel pieces:
 *  - the envelope recurrence, which is serial per channel but independent
 *    across channels (process one channel per SIMD lane, reduce to the
 *    shared envelope with fmax, exactly like the scalar loop);
 *  - the gain application, which is per-sample and vectorizes directly
 *    (vector log10/pow approximations plus the channel multiply).
 * SSE2 is the baseline on x86-64; everything degrades to the scalar loops
 * elsewhere. The envelope path produces bit-identical output to the scalar
 * code (same operation order per lane); the gain path differs from libm
 * powf/log10f by < ~4 ulp on the inputs it sees here. */

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <media-io/audio-math.h>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define DYN_SIMD_SSE2 1
#include <emmintrin.h>
#else
#define DYN_SIMD_SSE2 0
#endif

/* -------------------------------------------------------- */
/* envelope: per-i fmax across channels of a one-pole filter   */

static inline void dyn_envelope_analyze_scalar(float *env_buf, float **bufs, size_t num_channels, uint32_t num_samples,
					       float attack_gain, float release_gain, float env_state)
{
	memset(env_buf, 0, num_samples * sizeof(env_buf[0]));
	for (size_t chan = 0; chan < num_channels; ++chan) {
		if (!bufs[chan])
			continue;

		float env = env_state;
		for (uint32_t i = 0; i < num_samples; ++i) {
			const float env_in = fabsf(bufs[chan][i]);
			const float coef = (env < env_in) ? attack_gain : release_gain;
			env = env_in + coef * (env - env_in);
			env_buf[i] = fmaxf(env_buf[i], env);
		}
	}
}

#if DYN_SIMD_SSE2

static inline float dyn_hmax4(__m128 v)
{
	__m128 hi = _mm_movehl_ps(v, v);
	v = _mm_max_ps(v, hi);
	__m128 sh = _mm_shuffle_ps(v, v, 0x01);
	return _mm_cvtss_f32(_mm_max_ss(v, sh));
}

static inline void dyn_envelope_group(float *env_buf, float **bufs, size_t chan_base, size_t chan_count,
				      uint32_t num_samples, __m128 attack, __m128 release, __m128 envv, __m128 neg_inf)
{
	__m128 valid;
	{
		float mask[4] = {0, 0, 0, 0};
		for (size_t c = 0; c < chan_count && c < 4; ++c)
			if (bufs[chan_base + c])
				mask[c] = 1.0f;
		valid = _mm_cmpgt_ps(_mm_loadu_ps(mask), _mm_setzero_ps());
	}

	const __m128 zero = _mm_setzero_ps();
	uint32_t i = 0;
	for (; i + 4 <= num_samples; i += 4) {
		/* 4x4 transpose: row c = |bufs[chan_base+c][i..i+3]| becomes
		 * i-vectors whose lanes are channels. */
		__m128 r[4];
		for (size_t c = 0; c < 4; ++c) {
			const float *p = (c < chan_count && bufs[chan_base + c]) ? bufs[chan_base + c] + i : NULL;
			__m128 row = p ? _mm_loadu_ps(p) : zero;
			r[c] = _mm_andnot_ps(_mm_set1_ps(-0.0f), row);
		}
		_MM_TRANSPOSE4_PS(r[0], r[1], r[2], r[3]);

		for (int k = 0; k < 4; ++k) {
			const __m128 env_in = r[k];
			const __m128 m = _mm_cmplt_ps(envv, env_in);
			const __m128 coef = _mm_or_ps(_mm_and_ps(m, attack), _mm_andnot_ps(m, release));
			envv = _mm_add_ps(env_in, _mm_mul_ps(coef, _mm_sub_ps(envv, env_in)));
			const __m128 out = _mm_or_ps(_mm_and_ps(valid, envv), _mm_andnot_ps(valid, neg_inf));
			env_buf[i + k] = fmaxf(env_buf[i + k], dyn_hmax4(out));
		}
	}
	for (; i < num_samples; ++i) {
		float lane[4] = {0, 0, 0, 0};
		for (size_t c = 0; c < chan_count && c < 4; ++c)
			if (bufs[chan_base + c])
				lane[c] = fabsf(bufs[chan_base + c][i]);
		const __m128 env_in = _mm_loadu_ps(lane);
		const __m128 m = _mm_cmplt_ps(envv, env_in);
		const __m128 coef = _mm_or_ps(_mm_and_ps(m, attack), _mm_andnot_ps(m, release));
		envv = _mm_add_ps(env_in, _mm_mul_ps(coef, _mm_sub_ps(envv, env_in)));
		const __m128 out = _mm_or_ps(_mm_and_ps(valid, envv), _mm_andnot_ps(valid, neg_inf));
		env_buf[i] = fmaxf(env_buf[i], dyn_hmax4(out));
	}
}

static inline void dyn_envelope_analyze(float *env_buf, float **bufs, size_t num_channels, uint32_t num_samples,
					float attack_gain, float release_gain, float env_state)
{
	/* one live channel: the lane-packing overhead outweighs the win */
	if (num_channels < 2 || (num_channels == 2 && !bufs[1])) {
		dyn_envelope_analyze_scalar(env_buf, bufs, num_channels, num_samples, attack_gain, release_gain,
					    env_state);
		return;
	}
	memset(env_buf, 0, num_samples * sizeof(env_buf[0]));
	const __m128 attack = _mm_set1_ps(attack_gain);
	const __m128 release = _mm_set1_ps(release_gain);
	const __m128 envv0 = _mm_set1_ps(env_state);
	const __m128 neg_inf = _mm_set1_ps(-INFINITY);
	/* Each group re-starts from the caller's state, matching the scalar
	 * loop, which re-reads the same state per channel. Up to two groups
	 * cover MAX_AUDIO_CHANNELS == 8. */
	for (size_t base = 0; base < num_channels; base += 4) {
		const size_t cnt = num_channels - base;
		dyn_envelope_group(env_buf, bufs, base, cnt < 4 ? cnt : 4, num_samples, attack, release, envv0,
				   neg_inf);
	}
}

#else /* !DYN_SIMD_SSE2 */

static inline void dyn_envelope_analyze(float *env_buf, float **bufs, size_t num_channels, uint32_t num_samples,
					float attack_gain, float release_gain, float env_state)
{
	dyn_envelope_analyze_scalar(env_buf, bufs, num_channels, num_samples, attack_gain, release_gain, env_state);
}

#endif /* DYN_SIMD_SSE2 */

/* -------------------------------------------------------- */
/* gain pass: vector log10f / 10**(x) (Cephes-style polys)     */

#if DYN_SIMD_SSE2

/* natural log of a positive __m128; callers mask non-positive lanes */
static inline __m128 dyn_logf_ps(__m128 x)
{
	const __m128 one = _mm_set1_ps(1.0f);
	__m128i ix = _mm_castps_si128(x);
	__m128i ei = _mm_srli_epi32(ix, 23);
	ix = _mm_and_si128(ix, _mm_set1_epi32(0x007fffff));
	ix = _mm_or_si128(ix, _mm_castps_si128(one));
	x = _mm_castsi128_ps(ix);
	__m128 e = _mm_cvtepi32_ps(_mm_sub_epi32(ei, _mm_set1_epi32(127)));

	const __m128 sqrthf = _mm_set1_ps(0.707106781186547524f);
	__m128 m = _mm_cmplt_ps(x, sqrthf);
	x = _mm_add_ps(x, _mm_and_ps(m, x));
	e = _mm_sub_ps(e, _mm_and_ps(m, one));
	__m128 f = _mm_sub_ps(x, one);
	__m128 s = _mm_mul_ps(f, _mm_mul_ps(f, f));

	__m128 y = _mm_set1_ps(7.0376836292e-2f);
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(-1.1514610310e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(1.1676998740e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(-1.2420140846e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(1.4249322787e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(-1.6668057665e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(2.0000714765e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(-2.4999993993e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, f), _mm_set1_ps(3.3333331174e-1f));
	y = _mm_mul_ps(y, s);

	y = _mm_add_ps(y, _mm_mul_ps(e, _mm_set1_ps(-2.12194440e-4f)));
	y = _mm_sub_ps(y, _mm_mul_ps(_mm_set1_ps(0.5f), _mm_mul_ps(f, f)));
	__m128 r = _mm_add_ps(f, y);
	return _mm_add_ps(r, _mm_mul_ps(e, _mm_set1_ps(0.693359375f)));
}

/* 10**x elementwise; x expected <= 0 here, -inf clamps to 0 */
static inline __m128 dyn_pow10f_ps(__m128 x)
{
	/* 10**x = exp(x * ln 10) */
	x = _mm_mul_ps(x, _mm_set1_ps(2.302585092994046f));
	__m128 fx = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(1.4426950408889634f)), _mm_set1_ps(0.5f));
	__m128i ki = _mm_cvttps_epi32(fx);
	__m128 k = _mm_cvtepi32_ps(ki);
	__m128 over = _mm_cmpgt_ps(k, fx);
	k = _mm_sub_ps(k, _mm_and_ps(over, _mm_set1_ps(1.0f)));
	ki = _mm_sub_epi32(ki, _mm_srli_epi32(_mm_castps_si128(over), 31));

	x = _mm_sub_ps(x, _mm_mul_ps(k, _mm_set1_ps(0.693359375f)));
	x = _mm_sub_ps(x, _mm_mul_ps(k, _mm_set1_ps(-2.12194440e-4f)));

	__m128 y = _mm_set1_ps(1.9875691500e-4f);
	y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.3981999507e-3f));
	y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(8.3334519073e-3f));
	y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(4.1665795894e-2f));
	y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.6666665459e-1f));
	y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(5.0000001201e-1f));
	y = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(y, x), x), _mm_add_ps(x, _mm_set1_ps(1.0f)));

	ki = _mm_add_epi32(ki, _mm_set1_epi32(127));
	ki = _mm_slli_epi32(ki, 23);
	y = _mm_mul_ps(y, _mm_castsi128_ps(ki));

	/* -inf -> 0; +inf -> +inf (matches powf semantics) */
	__m128 bad = _mm_cmpeq_ps(x, _mm_set1_ps(-INFINITY));
	__m128 big = _mm_cmpeq_ps(x, _mm_set1_ps(INFINITY));
	y = _mm_andnot_ps(bad, y);
	return _mm_or_ps(_mm_and_ps(big, x), y);
}

/* limiter/compressor gain stage:
 *   env_db = mul_to_db(env_buf[i]); gain = db_to_mul(min(0, slope*(threshold-env_db)));
 *   samples[c][i] *= gain * output_gain
 * Vector part computes gains 4 at a time; channel multiply vectorizes per
 * channel. Non-positive envelope maps to -inf dB like mul_to_db. */
static inline void dyn_compression_gain_apply(float *env_buf, float **samples, size_t num_channels,
					      uint32_t num_samples, float slope, float threshold, float output_gain)
{
	const __m128 m20log10e = _mm_set1_ps(8.685889638065037f); /* 20*log10(e) */
	const __m128 slope_v = _mm_set1_ps(slope);
	const __m128 thr_v = _mm_set1_ps(threshold);
	const __m128 zero = _mm_setzero_ps();
	const __m128 out_gain_v = _mm_set1_ps(output_gain);

	uint32_t i = 0;
	for (; i + 4 <= num_samples; i += 4) {
		__m128 env = _mm_loadu_ps(env_buf + i);
		__m128 nonpos = _mm_cmple_ps(env, zero);
		__m128 env_db = _mm_mul_ps(dyn_logf_ps(_mm_max_ps(env, _mm_set1_ps(1e-30f))), m20log10e);
		env_db = _mm_or_ps(_mm_and_ps(nonpos, _mm_set1_ps(-INFINITY)), _mm_andnot_ps(nonpos, env_db));
		/* gain_db = min(0, slope*(thr - env_db)) ; -inf env_db -> +inf -> 0 */
		__m128 gain_db = _mm_mul_ps(slope_v, _mm_sub_ps(thr_v, env_db));
		gain_db = _mm_min_ps(zero, gain_db);
		__m128 gain = _mm_mul_ps(dyn_pow10f_ps(_mm_mul_ps(gain_db, _mm_set1_ps(0.05f))), out_gain_v);
		for (size_t c = 0; c < num_channels; ++c) {
			if (!samples[c])
				continue;
			__m128 s = _mm_loadu_ps(samples[c] + i);
			_mm_storeu_ps(samples[c] + i, _mm_mul_ps(s, gain));
		}
	}
	for (; i < num_samples; ++i) {
		const float env_db = mul_to_db(env_buf[i]);
		float gain = slope * (threshold - env_db);
		gain = db_to_mul(fminf(0, gain)) * output_gain;
		for (size_t c = 0; c < num_channels; ++c)
			if (samples[c])
				samples[c][i] *= gain;
	}
}

#endif /* DYN_SIMD_SSE2 */

/* -------------------------------------------------------- */
/* noise gate: per-i level = max over channels of |x|, and      */
/* per-channel gain application from a per-i multiplier curve   */

static inline void dyn_channel_max_abs(float *out, float **bufs, size_t num_channels, uint32_t num_samples)
{
	memset(out, 0, num_samples * sizeof(out[0]));
#if DYN_SIMD_SSE2
	const __m128 sign = _mm_set1_ps(-0.0f);
	for (size_t c = 0; c < num_channels; ++c) {
		if (!bufs[c])
			continue;
		const float *in = bufs[c];
		uint32_t i = 0;
		for (; i + 4 <= num_samples; i += 4) {
			__m128 a = _mm_andnot_ps(sign, _mm_loadu_ps(in + i));
			__m128 m = _mm_loadu_ps(out + i);
			_mm_storeu_ps(out + i, _mm_max_ps(m, a));
		}
		for (; i < num_samples; ++i)
			out[i] = fmaxf(out[i], fabsf(in[i]));
	}
#else
	for (size_t c = 0; c < num_channels; ++c) {
		if (!bufs[c])
			continue;
		for (uint32_t i = 0; i < num_samples; ++i)
			out[i] = fmaxf(out[i], fabsf(bufs[c][i]));
	}
#endif
}

static inline void dyn_apply_gain(float **bufs, size_t num_channels, uint32_t num_samples, const float *gain)
{
#if DYN_SIMD_SSE2
	for (size_t c = 0; c < num_channels; ++c) {
		if (!bufs[c])
			continue;
		float *out = bufs[c];
		uint32_t i = 0;
		for (; i + 4 <= num_samples; i += 4)
			_mm_storeu_ps(out + i, _mm_mul_ps(_mm_loadu_ps(out + i), _mm_loadu_ps(gain + i)));
		for (; i < num_samples; ++i)
			out[i] *= gain[i];
	}
#else
	for (size_t c = 0; c < num_channels; ++c) {
		if (!bufs[c])
			continue;
		for (uint32_t i = 0; i < num_samples; ++i)
			bufs[c][i] *= gain[i];
	}
#endif
}

/* -------------------------------------------------------- */
/* expander/upward compressor: three passes over one channel.  */
/* gain_db[] first receives the per-i desired gain in dB, is   */
/* rewritten in place by the serial ballistics pass, then the  */
/* output multiply applies db_to_mul(gain_db) * output_gain.   */

struct dyn_expander_cfg {
	bool is_upwcomp;
	float threshold;
	float slope;
	float knee;
	float attack_gain;
	float inv_attack_gain;
	float release_gain;
	float inv_release_gain;
	float output_gain;
};

static inline void dyn_expander_process(float *env_buf, float *gain_db, float *samples, uint32_t num_samples,
					float channel_gain, const struct dyn_expander_cfg *cfg)
{
#if DYN_SIMD_SSE2
	const __m128 m20log10e = _mm_set1_ps(8.685889638065037f);
	const __m128 zero = _mm_setzero_ps();
	const __m128 thr = _mm_set1_ps(cfg->threshold);
	const __m128 slope_v = _mm_set1_ps(cfg->slope);
	const __m128 kh = _mm_set1_ps(cfg->knee * 0.5f);
	const __m128 kneeb = _mm_set1_ps((cfg->threshold - 60.0f) * 0.5f);
	const __m128 p60 = _mm_set1_ps(60.0f);
	const __m128 n60 = _mm_set1_ps(-60.0f);
	const __m128 inv2k = _mm_set1_ps(0.5f / cfg->knee);

	uint32_t i = 0;
	for (; i + 4 <= num_samples; i += 4) {
		__m128 env = _mm_loadu_ps(env_buf + i);
		__m128 nonpos = _mm_cmple_ps(env, zero);
		__m128 env_db = _mm_mul_ps(dyn_logf_ps(_mm_max_ps(env, _mm_set1_ps(1e-30f))), m20log10e);
		env_db = _mm_or_ps(_mm_and_ps(nonpos, _mm_set1_ps(-INFINITY)), _mm_andnot_ps(nonpos, env_db));
		__m128 diff = _mm_sub_ps(thr, env_db);
		__m128 target;
		if (cfg->is_upwcomp) {
			/* far below threshold: diff = max(env_db + 60, 0) */
			__m128 deep = _mm_cmple_ps(env_db, kneeb);
			__m128 dd = _mm_max_ps(_mm_add_ps(env_db, p60), zero);
			diff = _mm_or_ps(_mm_and_ps(deep, dd), _mm_andnot_ps(deep, diff));
			__m128 above = _mm_cmpge_ps(env_db, _mm_add_ps(thr, kh));
			__m128 below = _mm_cmpge_ps(_mm_sub_ps(thr, kh), env_db);
			__m128 d = _mm_add_ps(diff, kh);
			__m128 mid = _mm_mul_ps(_mm_mul_ps(slope_v, _mm_mul_ps(d, d)), inv2k);
			target = _mm_mul_ps(slope_v, diff);
			target = _mm_or_ps(_mm_and_ps(below, target), _mm_andnot_ps(below, mid));
			target = _mm_andnot_ps(above, target);
		} else {
			__m128 pos = _mm_cmpgt_ps(diff, zero);
			target = _mm_and_ps(pos, _mm_max_ps(_mm_mul_ps(slope_v, diff), n60));
		}
		_mm_storeu_ps(gain_db + i, target);
	}
	for (; i < num_samples; ++i) {
		const float env_db = mul_to_db(env_buf[i]);
		float diff = cfg->threshold - env_db;
		if (cfg->is_upwcomp && env_db <= (cfg->threshold - 60.0f) / 2)
			diff = env_db + 60.0f > 0 ? env_db + 60.0f : 0.0f;
		float target = 0.0f;
		if (cfg->is_upwcomp) {
			if (env_db >= cfg->threshold + cfg->knee / 2)
				target = 0.0f;
			if (cfg->threshold - cfg->knee / 2 >= env_db)
				target = cfg->slope * diff;
			if (env_db > cfg->threshold - cfg->knee / 2 && cfg->threshold + cfg->knee / 2 > env_db) {
				const float d = diff + cfg->knee / 2;
				target = cfg->slope * (d * d) / (2.0f * cfg->knee);
			}
		} else {
			target = diff > 0.0f ? fmaxf(cfg->slope * diff, -60.0f) : 0.0f;
		}
		gain_db[i] = target;
	}

	/* serial ballistics, identical to the scalar loop */
	float prev = channel_gain;
	for (i = 0; i < num_samples; ++i) {
		const float target = gain_db[i];
		float g;
		if (cfg->is_upwcomp) {
			prev = fmaxf(prev, 0.0f);
			g = target;
		} else {
			g = target;
		}
		if (g > prev)
			g = cfg->attack_gain * prev + cfg->inv_attack_gain * g;
		else
			g = cfg->release_gain * prev + cfg->inv_release_gain * g;
		gain_db[i] = g;
		prev = g;
	}

	/* output multiply, vectorized */
	{
		const __m128 og = _mm_set1_ps(cfg->output_gain);
		i = 0;
		for (; i + 4 <= num_samples; i += 4) {
			__m128 gd = _mm_loadu_ps(gain_db + i);
			if (!cfg->is_upwcomp)
				gd = _mm_min_ps(zero, gd);
			__m128 gain = _mm_mul_ps(dyn_pow10f_ps(_mm_mul_ps(gd, _mm_set1_ps(0.05f))), og);
			_mm_storeu_ps(samples + i, _mm_mul_ps(_mm_loadu_ps(samples + i), gain));
		}
		for (; i < num_samples; ++i) {
			float gd = gain_db[i];
			if (!cfg->is_upwcomp)
				gd = fminf(0, gd);
			samples[i] *= db_to_mul(gd) * cfg->output_gain;
		}
	}
#else
	for (uint32_t i = 0; i < num_samples; ++i) {
		const float env_db = mul_to_db(env_buf[i]);
		float diff = cfg->threshold - env_db;
		if (cfg->is_upwcomp && env_db <= (cfg->threshold - 60.0f) / 2)
			diff = env_db + 60.0f > 0 ? env_db + 60.0f : 0.0f;
		float target = 0.0f;
		if (cfg->is_upwcomp) {
			if (env_db >= cfg->threshold + cfg->knee / 2)
				target = 0.0f;
			if (cfg->threshold - cfg->knee / 2 >= env_db)
				target = cfg->slope * diff;
			if (env_db > cfg->threshold - cfg->knee / 2 && cfg->threshold + cfg->knee / 2 > env_db) {
				const float d = diff + cfg->knee / 2;
				target = cfg->slope * (d * d) / (2.0f * cfg->knee);
			}
		} else {
			target = diff > 0.0f ? fmaxf(cfg->slope * diff, -60.0f) : 0.0f;
		}
		const float prev = i > 0 ? (cfg->is_upwcomp ? fmaxf(gain_db[i - 1], 0) : gain_db[i - 1])
					 : (cfg->is_upwcomp ? fmaxf(channel_gain, 0) : channel_gain);
		if (target > prev)
			gain_db[i] = cfg->attack_gain * prev + cfg->inv_attack_gain * target;
		else
			gain_db[i] = cfg->release_gain * prev + cfg->inv_release_gain * target;
		const float g = db_to_mul(cfg->is_upwcomp ? gain_db[i] : fminf(0, gain_db[i]));
		samples[i] *= g * cfg->output_gain;
	}
#endif
}

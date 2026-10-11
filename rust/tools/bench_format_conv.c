/* Microbench for the C conversion paths at 1080p (P3 baseline, issue #21).
 * Times the repo's format-conversion.c kernels and the live swscale path
 * (dlopen'd — no dev headers needed) over enough iterations for a stable
 * ns/frame figure. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The repo's kernels (link the real format-conversion.c). */
void compress_uyvx_to_i420(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
			   uint8_t *output[], const uint32_t out_linesize[]);
void compress_uyvx_to_nv12(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
			   uint8_t *output[], const uint32_t out_linesize[]);
void convert_uyvx_to_i444(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
			  uint8_t *output[], const uint32_t out_linesize[]);
void decompress_420(const uint8_t *const input[], const uint32_t in_linesize[], uint32_t start_y,
		    uint32_t end_y, uint8_t *output, uint32_t out_linesize);
void decompress_nv12(const uint8_t *const input[], const uint32_t in_linesize[], uint32_t start_y,
		     uint32_t end_y, uint8_t *output, uint32_t out_linesize);
void decompress_422(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
		    uint8_t *output, uint32_t out_linesize, bool leading_lum);

#define W 1920
#define H 1080
#define ITERS 200

static uint64_t ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void report(const char *name, uint64_t *samples, int n)
{
	qsort(samples, n, sizeof(*samples), cmp_u64);
	printf("%-28s median %8.0f ns/frame  (min %8.0f, p90 %8.0f)\n", name,
	       (double)samples[n / 2], (double)samples[0], (double)samples[(int)(n * 0.9)]);
}

/* ---- swscale via dlopen (AVPixelFormat values are stable ABI) ---- */
typedef void *(*get_ctx_t)(int, int, int, int, int, int, int, void *, void *, const double *);
typedef int (*scale_t)(void *, const uint8_t *const *, const int *, int, int, uint8_t *const *,
		       const int *);
typedef void (*free_ctx_t)(void *);

#define AV_PIX_FMT_YUV420P 0
#define AV_PIX_FMT_YUYV422 1
#define AV_PIX_FMT_UYVY422 15
#define SWS_BICUBIC 4

int main(void)
{
	uint8_t *uyvx = aligned_alloc(64, (size_t)W * H * 4);
	uint8_t *packed = aligned_alloc(64, (size_t)W * H * 4);
	uint8_t *y = aligned_alloc(64, (size_t)W * H);
	uint8_t *u = aligned_alloc(64, (size_t)W * H / 4);
	uint8_t *v = aligned_alloc(64, (size_t)W * H / 4);
	uint8_t *u4 = aligned_alloc(64, (size_t)W * H);
	uint8_t *v4 = aligned_alloc(64, (size_t)W * H);
	uint8_t *uv = aligned_alloc(64, (size_t)W * H / 2);
	if (!uyvx || !packed || !y || !u || !v || !u4 || !v4 || !uv)
		return 1;

	srand(12345);
	for (size_t i = 0; i < (size_t)W * H * 4; i++)
		uyvx[i] = rand() & 0xff;
	for (size_t i = 0; i < (size_t)W * H; i++) {
		y[i] = rand() & 0xff;
		u4[i] = rand() & 0xff;
		v4[i] = rand() & 0xff;
	}
	for (size_t i = 0; i < (size_t)W * H / 4; i++) {
		u[i] = rand() & 0xff;
		v[i] = rand() & 0xff;
	}
	for (size_t i = 0; i < (size_t)W * H / 2; i++)
		uv[i] = rand() & 0xff;

	uint32_t in_ls_uyvx = W * 4, out_ls_packed = W * 4;
	uint32_t ls420[3] = {W, W / 2, W / 2}, ls444[3] = {W, W, W}, lsnv12[2] = {W, W};
	uint32_t ls422i = W * 2, ls422o = W * 2;
	uint8_t *o420[3] = {y, u, v}, *o444[3] = {y, u4, v4}, *onv12[2] = {y, uv};
	const uint8_t *i420[3] = {y, u, v}, *inv12[2] = {y, uv};

	uint64_t s[ITERS];
	uint64_t t;

	for (int i = 0; i < ITERS; i++) {
		t = ns();
		compress_uyvx_to_i420(uyvx, in_ls_uyvx, 0, H, o420, ls420);
		s[i] = ns() - t;
	}
	report("compress_uyvx_to_i420", s, ITERS);
	for (int i = 0; i < ITERS; i++) {
		t = ns();
		compress_uyvx_to_nv12(uyvx, in_ls_uyvx, 0, H, onv12, lsnv12);
		s[i] = ns() - t;
	}
	report("compress_uyvx_to_nv12", s, ITERS);
	for (int i = 0; i < ITERS; i++) {
		t = ns();
		convert_uyvx_to_i444(uyvx, in_ls_uyvx, 0, H, o444, ls444);
		s[i] = ns() - t;
	}
	report("convert_uyvx_to_i444", s, ITERS);
	for (int i = 0; i < ITERS; i++) {
		t = ns();
		decompress_420(i420, ls420, 0, H, packed, out_ls_packed);
		s[i] = ns() - t;
	}
	report("decompress_420", s, ITERS);
	for (int i = 0; i < ITERS; i++) {
		t = ns();
		decompress_nv12(inv12, lsnv12, 0, H, packed, out_ls_packed);
		s[i] = ns() - t;
	}
	report("decompress_nv12", s, ITERS);
	for (int i = 0; i < ITERS; i++) {
		t = ns();
		decompress_422(uyvx, ls422i, 0, H, packed, ls422o, false);
		s[i] = ns() - t;
	}
	report("decompress_422", s, ITERS);

	/* Live path: swscale yuyv422 -> yuv420p, no resize. */
	void *sws = dlopen("libswscale.so.5", RTLD_NOW | RTLD_LOCAL);
	if (sws) {
		get_ctx_t get_ctx = (get_ctx_t)dlsym(sws, "sws_getContext");
		scale_t scale = (scale_t)dlsym(sws, "sws_scale");
		free_ctx_t free_ctx = (free_ctx_t)dlsym(sws, "sws_freeContext");
		if (get_ctx && scale && free_ctx) {
			uint8_t *yuyv = uyvx;
			const uint8_t *src[4] = {yuyv, NULL, NULL, NULL};
			int src_ls[4] = {W * 2, 0, 0, 0};
			uint8_t *dst[4] = {y, u, v, NULL};
			int dst_ls[4] = {W, W / 2, W / 2, 0};
			void *ctx = get_ctx(W, H, AV_PIX_FMT_YUYV422, W, H,
					    AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
			if (ctx) {
				for (int i = 0; i < ITERS; i++) {
					t = ns();
					scale(ctx, src, src_ls, 0, H, dst, dst_ls);
					s[i] = ns() - t;
				}
				report("sws yuyv422->yuv420p", s, ITERS);
				free_ctx(ctx);
			} else {
				puts("sws_getContext failed");
			}
		}
	} else {
		puts("libswscale.so.5 not loadable");
	}
	return 0;
}

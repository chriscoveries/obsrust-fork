// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Decisions behind the VAAPI encoder probe, kept free of libva so they can be
 * tested on their own. Opening a VAAPI display can cost most of a second (with
 * nvidia-vaapi-driver it initialises CUDA), so the probe avoids opening a
 * device it already asked, or one whose driver cannot encode at all. */

enum vaapi_codec_flags {
	VAAPI_CODEC_H264 = 1 << 0,
	VAAPI_CODEC_AV1 = 1 << 1,
	VAAPI_CODEC_HEVC = 1 << 2,
};

/* Whether the VA driver libva picks for a DRM device can only decode.
 * driver_link is the device's /sys/class/drm/<node>/device/driver link target
 * (its last component names the kernel driver); libva_driver_name is
 * LIBVA_DRIVER_NAME, which picks another VA driver, or NULL. For the nvidia
 * kernel driver libva loads nvidia-vaapi-driver, whose README says "Hardware
 * decoding only, encoding is not supported". */
bool vaapi_driver_is_decode_only(const char *driver_link, const char *libva_driver_name);

typedef uint32_t (*vaapi_probe_fn)(const char *device_path, void *param);

struct vaapi_caps_entry;

/* The codecs (enum vaapi_codec_flags) each device path supports, probed once
 * per path. Not thread-safe: the caller locks. */
struct vaapi_caps_cache {
	struct vaapi_caps_entry *first;
};

/* The flags for device_path, from the cache or from probe (then kept). */
uint32_t vaapi_caps_cache_get(struct vaapi_caps_cache *cache, const char *device_path, vaapi_probe_fn probe,
			      void *param);
/* Whether device_path has been probed. */
bool vaapi_caps_cache_has(const struct vaapi_caps_cache *cache, const char *device_path);
/* Keep flags for device_path, unless it already has an answer. */
void vaapi_caps_cache_add(struct vaapi_caps_cache *cache, const char *device_path, uint32_t flags);
void vaapi_caps_cache_free(struct vaapi_caps_cache *cache);

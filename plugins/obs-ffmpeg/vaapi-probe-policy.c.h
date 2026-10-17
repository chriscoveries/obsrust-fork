// SPDX-License-Identifier: GPL-2.0-or-later

#include "build/obs-ffmpeg-vaapi-probe-policy.pch.h"

struct vaapi_caps_entry {
	char *path;
	uint32_t flags;
	struct vaapi_caps_entry *next;
};

bool vaapi_driver_is_decode_only(const char *driver_link, const char *libva_driver_name)
{
	if (libva_driver_name || !driver_link)
		return false;

	const char *driver = strrchr(driver_link, '/');
	driver = driver ? driver + 1 : driver_link;
	return strcmp(driver, "nvidia") == 0;
}

static struct vaapi_caps_entry *find_entry(const struct vaapi_caps_cache *cache, const char *device_path)
{
	struct vaapi_caps_entry *entry = cache->first;
	while (entry && strcmp(entry->path, device_path) != 0)
		entry = entry->next;
	return entry;
}

bool vaapi_caps_cache_has(const struct vaapi_caps_cache *cache, const char *device_path)
{
	return find_entry(cache, device_path) != NULL;
}

void vaapi_caps_cache_add(struct vaapi_caps_cache *cache, const char *device_path, uint32_t flags)
{
	if (find_entry(cache, device_path))
		return;

	struct vaapi_caps_entry *entry = bzalloc(sizeof(*entry));
	entry->path = bstrdup(device_path);
	entry->flags = flags;
	entry->next = cache->first;
	cache->first = entry;
}

uint32_t vaapi_caps_cache_get(struct vaapi_caps_cache *cache, const char *device_path, vaapi_probe_fn probe,
			      void *param)
{
	struct vaapi_caps_entry *entry = find_entry(cache, device_path);
	if (!entry) {
		vaapi_caps_cache_add(cache, device_path, probe(device_path, param));
		entry = cache->first;
	}
	return entry->flags;
}

void vaapi_caps_cache_free(struct vaapi_caps_cache *cache)
{
	while (cache->first) {
		struct vaapi_caps_entry *next = cache->first->next;
		bfree(cache->first->path);
		bfree(cache->first);
		cache->first = next;
	}
}

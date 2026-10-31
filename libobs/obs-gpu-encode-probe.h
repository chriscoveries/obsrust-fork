/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>

struct obs_gpu_encode_probe_ops {
	bool (*sample_probe)(void *data);
	void (*fallback)(void *data);
	void (*stop_worker)(void *data);
	void (*free_resources)(void *data);
};

/* Shared by start_gpu_encode() and the lifecycle regression test. A failed
 * async sample probe must stop the worker before freeing its resources. */
static inline bool obs_gpu_encode_probe_and_cleanup(bool initialized, bool async_texture, bool created_resources,
						    const struct obs_gpu_encode_probe_ops *ops, void *data)
{
	bool success = initialized;
	if (success && async_texture)
		success = ops->sample_probe(data);

	if (async_texture && !success)
		ops->fallback(data);

	if (!success && created_resources) {
		ops->stop_worker(data);
		ops->free_resources(data);
	}

	return success;
}

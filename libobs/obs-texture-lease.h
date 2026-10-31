/******************************************************************************
    Copyright (C) 2026 by Chris Coveries

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum obs_texture_lease_phase {
	OBS_TEXTURE_LEASE_AVAILABLE,
	OBS_TEXTURE_LEASE_ACTIVE,
	OBS_TEXTURE_LEASE_RETIRED,
};

struct obs_texture_lease_state {
	size_t outstanding;
	enum obs_texture_lease_phase phase;
};

/* One token represents one asynchronous consumer of a texture frame. */
struct obs_texture_lease_token {
	struct obs_texture_lease_state *state;
	bool active;
};

enum obs_texture_lease_result {
	OBS_TEXTURE_LEASE_INVALID,
	OBS_TEXTURE_LEASE_HELD,
	OBS_TEXTURE_LEASE_RECYCLED,
};

enum obs_texture_pool_exhaustion_action {
	OBS_TEXTURE_POOL_DROP_NEWEST,
	OBS_TEXTURE_POOL_REPEAT_LAST,
};

void obs_texture_lease_init(struct obs_texture_lease_state *state);
bool obs_texture_lease_begin_frame(struct obs_texture_lease_state *state);
bool obs_texture_lease_acquire(struct obs_texture_lease_state *state, struct obs_texture_lease_token *token);
enum obs_texture_lease_result obs_texture_lease_release(struct obs_texture_lease_token *token);
enum obs_texture_lease_result obs_texture_lease_retire(struct obs_texture_lease_state *state);
bool obs_texture_lease_is_drained(const struct obs_texture_lease_state *state);
enum obs_texture_pool_exhaustion_action obs_texture_lease_pool_exhaustion(bool queued_frame_available);

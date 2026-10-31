/******************************************************************************
    Copyright (C) 2026 by Chris Coveries

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "libobs/build/libobs.pch.h"

void obs_texture_lease_init(struct obs_texture_lease_state *state)
{
	state->outstanding = 0;
	state->phase = OBS_TEXTURE_LEASE_AVAILABLE;
}

bool obs_texture_lease_begin_frame(struct obs_texture_lease_state *state)
{
	if (!state || state->phase != OBS_TEXTURE_LEASE_AVAILABLE || state->outstanding != 0)
		return false;

	state->phase = OBS_TEXTURE_LEASE_ACTIVE;
	return true;
}

bool obs_texture_lease_acquire(struct obs_texture_lease_state *state, struct obs_texture_lease_token *token)
{
	if (!state || !token || token->active || state->phase != OBS_TEXTURE_LEASE_ACTIVE ||
	    state->outstanding == SIZE_MAX)
		return false;

	token->state = state;
	token->active = true;
	state->outstanding++;
	return true;
}

enum obs_texture_lease_result obs_texture_lease_release(struct obs_texture_lease_token *token)
{
	if (!token || !token->active || !token->state)
		return OBS_TEXTURE_LEASE_INVALID;

	struct obs_texture_lease_state *state = token->state;
	token->active = false;
	token->state = NULL;

	if (!state->outstanding)
		return OBS_TEXTURE_LEASE_INVALID;

	state->outstanding--;
	if (state->phase == OBS_TEXTURE_LEASE_RETIRED && !state->outstanding) {
		state->phase = OBS_TEXTURE_LEASE_AVAILABLE;
		return OBS_TEXTURE_LEASE_RECYCLED;
	}

	return OBS_TEXTURE_LEASE_HELD;
}

enum obs_texture_lease_result obs_texture_lease_retire(struct obs_texture_lease_state *state)
{
	if (!state || state->phase != OBS_TEXTURE_LEASE_ACTIVE)
		return OBS_TEXTURE_LEASE_INVALID;

	if (!state->outstanding) {
		state->phase = OBS_TEXTURE_LEASE_AVAILABLE;
		return OBS_TEXTURE_LEASE_RECYCLED;
	}

	state->phase = OBS_TEXTURE_LEASE_RETIRED;
	return OBS_TEXTURE_LEASE_HELD;
}

bool obs_texture_lease_is_drained(const struct obs_texture_lease_state *state)
{
	return state && state->phase == OBS_TEXTURE_LEASE_AVAILABLE && !state->outstanding;
}

enum obs_texture_pool_exhaustion_action obs_texture_lease_pool_exhaustion(bool queued_frame_available)
{
	return queued_frame_available ? OBS_TEXTURE_POOL_REPEAT_LAST : OBS_TEXTURE_POOL_DROP_NEWEST;
}

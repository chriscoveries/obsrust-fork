#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

static inline bool preflight_graphics_effects_available(const char *test_name)
{
	/* On macOS Cocoa resolves these from the loaded framework's Resources.
	 * The Rust-linked Tier 2 framework may not contain them yet. */
	static const char *const required_effects[] = {
		"default.effect",
		"default_rect.effect",
		"opaque.effect",
		"solid.effect",
		"repeat.effect",
		"format_conversion.effect",
		"bicubic_scale.effect",
		"lanczos_scale.effect",
		"area.effect",
		"bilinear_lowres_scale.effect",
		"premultiplied_alpha.effect",
	};

	for (size_t i = 0; i < sizeof(required_effects) / sizeof(required_effects[0]); ++i) {
		char *path = obs_find_data_file(required_effects[i]);
		const bool available = path && os_file_exists(path);
		if (!available) {
			fprintf(stderr, "%s: SKIP: missing libobs graphics effect %s (%s)\n", test_name,
				required_effects[i], path ? path : "path unavailable");
			bfree(path);
			return false;
		}
		bfree(path);
	}

	return true;
}

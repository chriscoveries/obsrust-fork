/* Precompiled header (BUILD.md) for the plugins/win-dshow unity groups
 * win-dshow.decode-windows. C. Every implementation file in them includes this header and
 * nothing else: the includes and macro setup below are theirs, in group and
 * file order. */

#pragma once

/* ffmpeg-decode.c */
#include "../ffmpeg-decode.h"
#include "obs-ffmpeg-compat.h"
#include <obs-avc.h>
#ifdef ENABLE_HEVC
#include <obs-hevc.h>
#endif

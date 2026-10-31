/* Precompiled header (BUILD.md) for the plugins/mac-videotoolbox unity groups
 * mac-videotoolbox.macos. C. Every implementation file in them includes this header and
 * nothing else: the includes and macro setup below are theirs, in group and
 * file order. */

#pragma once

/* encoder.c */
#include <obs-module.h>
#include <util/darray.h>
#include <util/platform.h>
#include <obs-avc.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <VideoToolbox/VideoToolbox.h>
#include <VideoToolbox/VTVideoEncoderList.h>
#include <CoreMedia/CoreMedia.h>

#include <pthread.h>
#include <stdint.h>

#include <util/apple/cfstring-utils.h>

#include <assert.h>

#include "plugins/mac-videotoolbox/vt-frame-copy.h"

/* vt-frame-copy.c */
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/resource.h>

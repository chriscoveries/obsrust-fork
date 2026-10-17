/* Precompiled header (BUILD.md) for the plugins/obs-ffmpeg unity groups
 * obs-ffmpeg.vaapi. C. Every implementation file in them includes this header and
 * nothing else: the includes and macro setup below are theirs, in group and
 * file order. */

#pragma once

/* obs-ffmpeg-vaapi.c */
#include <libavutil/avutil.h>

#include <util/darray.h>
#include <util/dstr.h>
#include <util/base.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <obs-avc.h>
#include <obs-av1.h>
#ifdef ENABLE_HEVC
#include <obs-hevc.h>
#endif
#include <opts-parser.h>

#include <unistd.h>

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>

#include <pci/pci.h>

#include "../vaapi-utils.h"
#include "../vaapi-device-select.h"
#include "../obs-ffmpeg-formats.h"

#include <va/va_drmcommon.h>
#include <libdrm/drm_fourcc.h>

/* vaapi-utils.c */

#include <util/bmem.h>

#include <va/va_drm.h>
#include <va/va_str.h>

#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../vaapi-probe-policy.h"

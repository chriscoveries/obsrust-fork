/* Byte-counted plane copy for the VideoToolbox encoder upload path.
 *
 * Extracted from vt_encode() so the Mac zero-copy epic (fork #47) can pin
 * the per-frame CPU copy count: this function IS the copy path baseline
 * (~3 MB per 1080p NV12 frame). The M1 zero-copy path bypasses it; the
 * counter (its return value) must read 0 there for the same input.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <VideoToolbox/VideoToolbox.h>

#include <media-io/media-io-defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copies encoder frame planes into a locked pixel buffer, stopping at the
 * first NULL plane exactly like vt_encode() always has. `pixbuf` must be
 * locked. Returns bytes copied. */
size_t vt_copy_frame_to_pixbuf(CVPixelBufferRef pixbuf, uint8_t *data[MAX_AV_PLANES], uint32_t linesize[MAX_AV_PLANES]);

#ifdef __cplusplus
}
#endif

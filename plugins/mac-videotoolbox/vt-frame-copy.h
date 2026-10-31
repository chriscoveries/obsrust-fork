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
#include <util/c99defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copies encoder frame planes into a locked pixel buffer, stopping at the
 * first NULL plane exactly like vt_encode() always has. `pixbuf` must be
 * locked. Returns bytes copied. */
EXPORT size_t vt_copy_frame_to_pixbuf(CVPixelBufferRef pixbuf, uint8_t *data[MAX_AV_PLANES],
				      uint32_t linesize[MAX_AV_PLANES]);

/* Process-wide cumulative bytes copied by vt_copy_frame_to_pixbuf, for the
 * measurement harness. The IOSurface texture path leaves it at 0. */
EXPORT void vt_copy_bytes_reset(void);
EXPORT uint64_t vt_copy_bytes_total(void);

/* Harness controls and counters for matched VideoToolbox submissions. Plane
 * copy bytes are committed only when the corresponding VideoToolbox submit is
 * accepted; cancelled submissions discard their staged copy count. */
EXPORT void vt_encode_submission_reset(uint64_t limit);
EXPORT uint64_t vt_encode_submission_total(void);
EXPORT bool vt_encode_submission_reserve(void);
EXPORT void vt_encode_submission_commit(void);
EXPORT void vt_encode_submission_cancel(void);
EXPORT void vt_test_force_raw(bool force_raw);
EXPORT bool vt_test_raw_forced(void);

/* Harness timing. vt_encode_timing_reset() clears the samples and sets the
 * accepted submission at which the CPU window opens; it closes at the
 * submission limit. Each accepted frame's output callback records two
 * latencies: from entering the encode callback (vt_encode or
 * vt_encode_texture) and from the VideoToolbox submit, both to the output
 * callback. The window reads process user and system time
 * (getrusage(RUSAGE_SELF)) on the encode thread at its two submissions. */
EXPORT void vt_encode_timing_reset(uint64_t window_start);
EXPORT uint64_t vt_encode_timing_now(void);
EXPORT void vt_encode_timing_record(uint64_t entered_ns, uint64_t submitted_ns);
EXPORT size_t vt_encode_timing_samples(uint64_t *entry_to_output_ns, uint64_t *submit_to_output_ns, size_t capacity);
EXPORT bool vt_encode_cpu_window(uint64_t *frames, uint64_t *wall_ns, uint64_t *user_ns, uint64_t *system_ns);

#ifdef __cplusplus
}
#endif

//! Runtime-dispatched planar<->packed conversion kernels (perf P3).
//!
//! Scalar cores replicate `libobs/media-io/format-conversion.c` bit-exactly;
//! `is_x86_feature_detected!` dispatch upgrades the hot compress paths to
//! AVX2 (and AVX-512BW where present) at runtime. UYVX input layout is
//! [U, Y, V, X] per pixel; planar outputs are 8-bit.
//!
//! All entry points take raw pointers mirroring the C kernels; callers must
//! guarantee the buffers span `start_y..end_y` rows of the given linesizes.

#![forbid(unsafe_op_in_unsafe_fn)]

#[doc(hidden)]
pub mod scalar; // tests exercise each tier directly

#[cfg(target_arch = "x86_64")]
mod avx2;
#[cfg(target_arch = "x86_64")]
mod avx512;

/// Packed-444 UYVX -> planar I420. `output` = [y, u, v] planes.
///
/// # Safety
/// `input`/`output` must be valid for the given row range and linesizes.
pub unsafe fn compress_uyvx_to_i420(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 3],
    out_linesize: &[u32; 3],
) {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512bw") {
            // SAFETY: avx512bw presence checked above.
            return unsafe {
                avx512::compress_uyvx_to_i420(
                    input,
                    in_linesize,
                    start_y,
                    end_y,
                    output,
                    out_linesize,
                )
            };
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            // SAFETY: avx2 presence checked above.
            return unsafe {
                avx2::compress_uyvx_to_i420(
                    input,
                    in_linesize,
                    start_y,
                    end_y,
                    output,
                    out_linesize,
                )
            };
        }
    }
    // SAFETY: forwarded caller guarantees.
    unsafe {
        scalar::compress_uyvx_to_i420(input, in_linesize, start_y, end_y, output, out_linesize)
    }
}

/// Packed-444 UYVX -> semi-planar NV12. `output` = [y, uv] planes.
///
/// # Safety
/// Same contract as [`compress_uyvx_to_i420`].
pub unsafe fn compress_uyvx_to_nv12(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 2],
    out_linesize: &[u32; 2],
) {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx2") {
            // SAFETY: avx2 presence checked above.
            return unsafe {
                avx2::compress_uyvx_to_nv12(
                    input,
                    in_linesize,
                    start_y,
                    end_y,
                    output,
                    out_linesize,
                )
            };
        }
    }
    // SAFETY: forwarded caller guarantees.
    unsafe {
        scalar::compress_uyvx_to_nv12(input, in_linesize, start_y, end_y, output, out_linesize)
    }
}

/// Packed-444 UYVX -> planar I444.
///
/// # Safety
/// Same contract as [`compress_uyvx_to_i420`].
pub unsafe fn convert_uyvx_to_i444(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 3],
    out_linesize: &[u32; 3],
) {
    // SAFETY: forwarded caller guarantees.
    unsafe {
        scalar::convert_uyvx_to_i444(input, in_linesize, start_y, end_y, output, out_linesize)
    }
}

/// Planar I420 -> packed [V, U, Y, 0] 444.
///
/// # Safety
/// Same contract as [`compress_uyvx_to_i420`]; `input` = [y, u, v].
pub unsafe fn decompress_420(
    input: &[*const u8; 3],
    in_linesize: &[u32; 3],
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
) {
    // SAFETY: forwarded caller guarantees.
    unsafe { scalar::decompress_420(input, in_linesize, start_y, end_y, output, out_linesize) }
}

/// Semi-planar NV12 -> packed [Y, U, V, 0] 444.
///
/// # Safety
/// Same contract; `input` = [y, uv].
pub unsafe fn decompress_nv12(
    input: &[*const u8; 2],
    in_linesize: &[u32; 2],
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
) {
    // SAFETY: forwarded caller guarantees.
    unsafe { scalar::decompress_nv12(input, in_linesize, start_y, end_y, output, out_linesize) }
}

/// Packed-422 -> packed-444 chroma expansion (`leading_lum` picks the byte
/// duplicated into the chroma slot).
///
/// # Safety
/// Same contract as [`compress_uyvx_to_i420`].
pub unsafe fn decompress_422(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
    leading_lum: bool,
) {
    // SAFETY: forwarded caller guarantees.
    unsafe {
        scalar::decompress_422(
            input,
            in_linesize,
            start_y,
            end_y,
            output,
            out_linesize,
            leading_lum,
        )
    }
}

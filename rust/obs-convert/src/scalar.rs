//! Scalar reference kernels, bit-exact with
//! `libobs/media-io/format-conversion.c`. UYVX pixels are little-endian u32
//! words `[U, Y, V, X]`; chroma averages are `sum of the 2x2 block >> 2`
//! (floor), matching the SSE `srai(2)` on the packed sum.

#![forbid(unsafe_op_in_unsafe_fn)]

#[inline(always)]
fn min_u32(a: u32, b: u32) -> u32 {
    if a < b { a } else { b }
}

/// UYVX -> I420: lum = byte 1 per px; 2x2 chroma average of byte 0 -> u,
/// byte 2 -> v, one sample per 2x2 block.
///
/// # Safety
/// Caller contract from `compress_uyvx_to_i420`.
pub unsafe fn compress_uyvx_to_i420(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 3],
    out_linesize: &[u32; 3],
) {
    let width = min_u32(in_linesize, out_linesize[0]);
    // SAFETY: forwarded caller guarantees.
    unsafe {
        compress_uyvx_to_i420_rows(
            input,
            in_linesize,
            start_y,
            end_y,
            output,
            out_linesize,
            0,
            width,
        )
    }
}

/// Row-pair body of `compress_uyvx_to_i420` restricted to `x_start..x_end`
/// columns (x range is a multiple of 4 px; `end_y - start_y` even). Used by
/// the wide-vector tiers for tail columns.
///
/// # Safety
/// Same contract as `compress_uyvx_to_i420`.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn compress_uyvx_to_i420_rows(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 3],
    out_linesize: &[u32; 3],
    x_start: u32,
    x_end: u32,
) {
    let lum = output[0];
    let u_plane = output[1];
    let v_plane = output[2];
    let mut y = start_y;
    while y < end_y {
        let mut x = x_start;
        while x < x_end {
            let base0 = (y as usize) * in_linesize as usize + (x as usize) * 4;
            let base1 = base0 + in_linesize as usize;
            let lum_y = (y as usize) * out_linesize[0] as usize + x as usize;
            let lum_y1 = lum_y + out_linesize[0] as usize;
            let chroma = ((y >> 1) as usize) * out_linesize[1] as usize + (x >> 1) as usize;
            for k in 0..4usize {
                // SAFETY: within the caller-guaranteed frame regions.
                unsafe {
                    *lum.add(lum_y + k) = *input.add(base0 + k * 4 + 1);
                    *lum.add(lum_y1 + k) = *input.add(base1 + k * 4 + 1);
                }
            }
            for pair in 0..2usize {
                let (a, b, c, d) = unsafe {
                    let i0 = base0 + (pair * 2) * 4;
                    let i1 = base0 + (pair * 2 + 1) * 4;
                    let i2 = base1 + (pair * 2) * 4;
                    let i3 = base1 + (pair * 2 + 1) * 4;
                    (
                        *input.add(i0) as u32,
                        *input.add(i1) as u32,
                        *input.add(i2) as u32,
                        *input.add(i3) as u32,
                    )
                };
                let (va, vb, vc, vd) = unsafe {
                    let i0 = base0 + (pair * 2) * 4 + 2;
                    let i1 = base0 + (pair * 2 + 1) * 4 + 2;
                    let i2 = base1 + (pair * 2) * 4 + 2;
                    let i3 = base1 + (pair * 2 + 1) * 4 + 2;
                    (
                        *input.add(i0) as u32,
                        *input.add(i1) as u32,
                        *input.add(i2) as u32,
                        *input.add(i3) as u32,
                    )
                };
                unsafe {
                    *u_plane.add(chroma + pair) = ((a + b + c + d) >> 2) as u8;
                    *v_plane.add(chroma + pair) = ((va + vb + vc + vd) >> 2) as u8;
                }
            }
            x += 4;
        }
        y += 2;
    }
}

/// UYVX -> NV12: lum as above; chroma plane interleaves [u,v] per 2x2 block.
///
/// # Safety
/// Caller contract from `compress_uyvx_to_nv12`.
pub unsafe fn compress_uyvx_to_nv12(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 2],
    out_linesize: &[u32; 2],
) {
    let lum = output[0];
    let chroma_plane = output[1];
    let width = min_u32(in_linesize, out_linesize[0]);
    let mut y = start_y;
    while y < end_y {
        let mut x = 0;
        while x < width {
            let base0 = (y as usize) * in_linesize as usize + (x as usize) * 4;
            let base1 = base0 + in_linesize as usize;
            let lum_y = (y as usize) * out_linesize[0] as usize + x as usize;
            let lum_y1 = lum_y + out_linesize[0] as usize;
            let chroma = ((y >> 1) as usize) * out_linesize[1] as usize + x as usize;
            for k in 0..4usize {
                // SAFETY: within the caller-guaranteed frame regions.
                unsafe {
                    *lum.add(lum_y + k) = *input.add(base0 + k * 4 + 1);
                    *lum.add(lum_y1 + k) = *input.add(base1 + k * 4 + 1);
                }
            }
            for pair in 0..2usize {
                let (ua, ub, uc, ud) = unsafe {
                    let i0 = base0 + (pair * 2) * 4;
                    (
                        *input.add(i0) as u32,
                        *input.add(i0 + 4) as u32,
                        *input.add(i0 + in_linesize as usize) as u32,
                        *input.add(i0 + in_linesize as usize + 4) as u32,
                    )
                };
                let (va, vb, vc, vd) = unsafe {
                    let i0 = base0 + (pair * 2) * 4 + 2;
                    (
                        *input.add(i0) as u32,
                        *input.add(i0 + 4) as u32,
                        *input.add(i0 + in_linesize as usize) as u32,
                        *input.add(i0 + in_linesize as usize + 4) as u32,
                    )
                };
                unsafe {
                    *chroma_plane.add(chroma + pair * 2) = ((ua + ub + uc + ud) >> 2) as u8;
                    *chroma_plane.add(chroma + pair * 2 + 1) = ((va + vb + vc + vd) >> 2) as u8;
                }
            }
            x += 4;
        }
        y += 2;
    }
}

/// UYVX -> I444: per-pixel extraction, no averaging.
///
/// # Safety
/// Caller contract from `convert_uyvx_to_i444`.
pub unsafe fn convert_uyvx_to_i444(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: &mut [*mut u8; 3],
    out_linesize: &[u32; 3],
) {
    let lum = output[0];
    let u_plane = output[1];
    let v_plane = output[2];
    let width = min_u32(in_linesize, out_linesize[0]);
    let mut y = start_y;
    while y < end_y {
        let mut x = 0;
        while x < width {
            let base0 = (y as usize) * in_linesize as usize + (x as usize) * 4;
            let base1 = base0 + in_linesize as usize;
            let lum_y = (y as usize) * out_linesize[0] as usize + x as usize;
            let lum_y1 = lum_y + out_linesize[0] as usize;
            for k in 0..4usize {
                // SAFETY: within the caller-guaranteed frame regions.
                unsafe {
                    let px0 = base0 + k * 4;
                    let px1 = base1 + k * 4;
                    *lum.add(lum_y + k) = *input.add(px0 + 1);
                    *lum.add(lum_y1 + k) = *input.add(px1 + 1);
                    *u_plane.add(lum_y + k) = *input.add(px0);
                    *u_plane.add(lum_y1 + k) = *input.add(px1);
                    *v_plane.add(lum_y + k) = *input.add(px0 + 2);
                    *v_plane.add(lum_y1 + k) = *input.add(px1 + 2);
                }
            }
            x += 4;
        }
        y += 2;
    }
}

/// I420 -> packed [V, U, Y, 0] 444; each chroma sample covers a 2x2 block.
///
/// # Safety
/// Caller contract from `decompress_420`.
pub unsafe fn decompress_420(
    input: &[*const u8; 3],
    in_linesize: &[u32; 3],
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
) {
    let width_d2 = in_linesize[0] / 2;
    let mut y = start_y / 2;
    while y < end_y / 2 {
        let mut x = 0;
        while x < width_d2 {
            // SAFETY: within the caller-guaranteed frame regions.
            unsafe {
                let u = *input[1].add(y as usize * in_linesize[1] as usize + x as usize) as u32;
                let v = *input[2].add(y as usize * in_linesize[2] as usize + x as usize) as u32;
                let out = (u << 8) | v;
                let lum0 = input[0].add(y as usize * 2 * in_linesize[0] as usize + 2 * x as usize);
                let lum1 = lum0.add(in_linesize[0] as usize);
                let o0 = (output as *mut u32)
                    .add(y as usize * 2 * (out_linesize / 4) as usize + 2 * x as usize);
                let o1 = o0.add((out_linesize / 4) as usize);
                *o0 = (*lum0 as u32) << 16 | out;
                *o0.add(1) = (*lum0.add(1) as u32) << 16 | out;
                *o1 = (*lum1 as u32) << 16 | out;
                *o1.add(1) = (*lum1.add(1) as u32) << 16 | out;
            }
            x += 1;
        }
        y += 1;
    }
}

/// NV12 -> packed [Y, U, V, 0] 444.
///
/// # Safety
/// Caller contract from `decompress_nv12`.
pub unsafe fn decompress_nv12(
    input: &[*const u8; 2],
    in_linesize: &[u32; 2],
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
) {
    let width_d2 = min_u32(in_linesize[0], out_linesize) / 2;
    let mut y = start_y / 2;
    while y < end_y / 2 {
        let mut x = 0;
        while x < width_d2 {
            // SAFETY: within the caller-guaranteed frame regions.
            unsafe {
                let chroma = (input[1].add(y as usize * in_linesize[1] as usize + 2 * x as usize)
                    as *const u16)
                    .read_unaligned() as u32;
                let out = chroma << 8;
                let lum0 = input[0].add(y as usize * 2 * in_linesize[0] as usize + 2 * x as usize);
                let lum1 = lum0.add(in_linesize[0] as usize);
                let o0 = (output as *mut u32)
                    .add(y as usize * 2 * (out_linesize / 4) as usize + 2 * x as usize);
                let o1 = o0.add((out_linesize / 4) as usize);
                *o0 = (*lum0 as u32) | out;
                *o0.add(1) = (*lum0.add(1) as u32) | out;
                *o1 = (*lum1 as u32) | out;
                *o1.add(1) = (*lum1.add(1) as u32) | out;
            }
            x += 1;
        }
        y += 1;
    }
}

/// Packed-422 -> packed-444 expansion, u32-word-wise.
///
/// # Safety
/// Caller contract from `decompress_422`.
pub unsafe fn decompress_422(
    input: *const u8,
    in_linesize: u32,
    start_y: u32,
    end_y: u32,
    output: *mut u8,
    out_linesize: u32,
    leading_lum: bool,
) {
    let width_d2 = min_u32(in_linesize, out_linesize) / 2;
    let mut y = start_y;
    while y < end_y {
        let mut i = 0;
        while i < width_d2 {
            // SAFETY: within the caller-guaranteed frame regions.
            unsafe {
                let dw = (input.add(y as usize * in_linesize as usize + 4 * i as usize)
                    as *const u32)
                    .read_unaligned();
                let o = (output.add(y as usize * out_linesize as usize) as *mut u32)
                    .add(2 * i as usize);
                *o = dw;
                *o.add(1) = if leading_lum {
                    (dw & 0xFFFFFF00) | ((dw >> 16) & 0xFF)
                } else {
                    (dw & 0xFFFF00FF) | ((dw >> 16) & 0xFF00)
                };
            }
            i += 1;
        }
        y += 1;
    }
}

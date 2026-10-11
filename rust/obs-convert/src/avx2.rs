//! AVX2 tier for the hot compress paths, selected at runtime by
//! `is_x86_feature_detected!`. Bit-exact with the scalar kernels: 2x2
//! chroma averages use `maddubs` pair-sums shifted right by 2 (floor),
//! never `avg_epu16` (which rounds up).

#![forbid(unsafe_op_in_unsafe_fn)]

#[cfg(target_arch = "x86_64")]
use core::arch::x86_64::*;

#[inline(always)]
fn min_u32(a: u32, b: u32) -> u32 {
    if a < b { a } else { b }
}

/// One 8-px UYVX row -> 8 lum bytes (byte 1 of each u32).
#[inline(always)]
unsafe fn luma8(row: *const u8) -> __m128i {
    unsafe {
        let v = _mm256_loadu_si256(row as *const __m256i);
        // Y sits at byte 1 of each u32: mask, shift into byte 0, pack.
        let y = _mm256_srli_epi32(_mm256_and_si256(v, _mm256_set1_epi32(0x0000FF00)), 8);
        let y16 = _mm256_packs_epi32(y, y); // u16 lanes, per 128-bit half
        let y8 = _mm256_packus_epi16(y16, y16);
        // lane [0..4] = px0-3, lane [8..12] = px4-7: gather via extract.
        let lo = _mm256_castsi256_si128(y8);
        let hi = _mm256_extracti128_si256(y8, 1);
        _mm_unpacklo_epi32(lo, hi)
    }
}

/// 2x2 chroma averages for 8 px across two rows of one UYVX byte channel.
/// Returns 4 u8s in the low u32: avg of px (0,1), (2,3), (4,5), (6,7).
#[inline(always)]
unsafe fn chroma4(row0: *const u8, row1: *const u8, byte_off: i32) -> u32 {
    unsafe {
        let m = if byte_off == 0 {
            _mm256_set1_epi32(0x000000FF)
        } else {
            _mm256_set1_epi32(0x00FF0000)
        };
        let sh = _mm_cvtsi32_si128(if byte_off == 0 { 0 } else { 16 });
        let a = _mm256_srl_epi32(
            _mm256_and_si256(_mm256_loadu_si256(row0 as *const __m256i), m),
            sh,
        );
        let b = _mm256_srl_epi32(
            _mm256_and_si256(_mm256_loadu_si256(row1 as *const __m256i), m),
            sh,
        );
        // u32 px values -> u8s (per-lane u32-0 = 4 px bytes), gather both
        // rows into [r0 px0-7, r1 px0-7], maddubs pair-sum, fold, >>2.
        let a8 = _mm256_packus_epi16(_mm256_packs_epi32(a, a), _mm256_setzero_si256());
        let b8 = _mm256_packus_epi16(_mm256_packs_epi32(b, b), _mm256_setzero_si256());
        let r0 = _mm_unpacklo_epi32(_mm256_castsi256_si128(a8), _mm256_extracti128_si256(a8, 1));
        let r1 = _mm_unpacklo_epi32(_mm256_castsi256_si128(b8), _mm256_extracti128_si256(b8, 1));
        let px = _mm_unpacklo_epi64(r0, r1); // [r0 px0-7, r1 px0-7]
        let sums = _mm_maddubs_epi16(px, _mm_set1_epi8(1)); // [r0p0-3, r1p0-3]
        let sums = _mm_add_epi16(sums, _mm_srli_si128(sums, 8)); // [pair0-3, ...]
        let avg = _mm_srli_epi16(sums, 2);
        _mm_cvtsi128_si32(_mm_packus_epi16(avg, avg)) as u32
    }
}

/// AVX2 UYVX -> I420.
///
/// # Safety
/// Requires AVX2; caller contract from `compress_uyvx_to_i420`.
#[target_feature(enable = "avx2")]
pub unsafe fn compress_uyvx_to_i420(
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
        // 8 px per iteration.
        while x + 8 <= width {
            let row0 = unsafe { input.add(y as usize * in_linesize as usize + (x as usize) * 4) };
            let row1 = unsafe { row0.add(in_linesize as usize) };
            let lum_y = y as usize * out_linesize[0] as usize + x as usize;
            unsafe {
                let l0 = luma8(row0);
                let l1 = luma8(row1);
                std::ptr::write_unaligned(lum.add(lum_y) as *mut u64, _mm_cvtsi128_si64(l0) as u64);
                std::ptr::write_unaligned(
                    lum.add(lum_y + out_linesize[0] as usize) as *mut u64,
                    _mm_cvtsi128_si64(l1) as u64,
                );
            }
            let u4 = unsafe { chroma4(row0, row1, 0) };
            let v4 = unsafe { chroma4(row0, row1, 2) };
            let chroma = (y >> 1) as usize * out_linesize[1] as usize + (x >> 1) as usize;
            unsafe {
                std::ptr::write_unaligned(u_plane.add(chroma) as *mut u32, u4);
                std::ptr::write_unaligned(v_plane.add(chroma) as *mut u32, v4);
            }
            x += 8;
        }
        // tail: scalar for x % 8 != 0 handled in 4-px chunks
        while x < width {
            let base0 = y as usize * in_linesize as usize + (x as usize) * 4;
            let base1 = base0 + in_linesize as usize;
            let lum_y = y as usize * out_linesize[0] as usize + x as usize;
            let lum_y1 = lum_y + out_linesize[0] as usize;
            let chroma = (y >> 1) as usize * out_linesize[1] as usize + (x >> 1) as usize;
            unsafe {
                for k in 0..4usize {
                    *lum.add(lum_y + k) = *input.add(base0 + k * 4 + 1);
                    *lum.add(lum_y1 + k) = *input.add(base1 + k * 4 + 1);
                }
                for pair in 0..2usize {
                    let i0 = base0 + (pair * 2) * 4;
                    let i2 = base1 + (pair * 2) * 4;
                    *u_plane.add(chroma + pair) = ((*input.add(i0) as u32
                        + *input.add(i0 + 4) as u32
                        + *input.add(i2) as u32
                        + *input.add(i2 + 4) as u32)
                        >> 2) as u8;
                    *v_plane.add(chroma + pair) = ((*input.add(i0 + 2) as u32
                        + *input.add(i0 + 6) as u32
                        + *input.add(i2 + 2) as u32
                        + *input.add(i2 + 6) as u32)
                        >> 2) as u8;
                }
            }
            x += 4;
        }
        y += 2;
    }
}

/// AVX2 UYVX -> NV12.
///
/// # Safety
/// Requires AVX2; caller contract from `compress_uyvx_to_nv12`.
#[target_feature(enable = "avx2")]
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
        while x + 8 <= width {
            let row0 = unsafe { input.add(y as usize * in_linesize as usize + (x as usize) * 4) };
            let row1 = unsafe { row0.add(in_linesize as usize) };
            let lum_y = y as usize * out_linesize[0] as usize + x as usize;
            unsafe {
                let l0 = luma8(row0);
                let l1 = luma8(row1);
                std::ptr::write_unaligned(lum.add(lum_y) as *mut u64, _mm_cvtsi128_si64(l0) as u64);
                std::ptr::write_unaligned(
                    lum.add(lum_y + out_linesize[0] as usize) as *mut u64,
                    _mm_cvtsi128_si64(l1) as u64,
                );
            }
            let u4 = unsafe { chroma4(row0, row1, 0) };
            let v4 = unsafe { chroma4(row0, row1, 2) };
            // interleave [u0,v0,u1,v1,u2,v2,u3,v3]
            let uv = (u4 as u64) | ((v4 as u64) << 32);
            let mut bytes = [0u8; 8];
            bytes.copy_from_slice(&uv.to_le_bytes());
            let mut inter = [0u8; 8];
            for i in 0..4 {
                inter[i * 2] = bytes[i];
                inter[i * 2 + 1] = bytes[4 + i];
            }
            let chroma = (y >> 1) as usize * out_linesize[1] as usize + x as usize;
            unsafe {
                std::ptr::write_unaligned(
                    chroma_plane.add(chroma) as *mut u64,
                    u64::from_le_bytes(inter),
                );
            }
            x += 8;
        }
        while x < width {
            let base0 = y as usize * in_linesize as usize + (x as usize) * 4;
            let base1 = base0 + in_linesize as usize;
            let lum_y = y as usize * out_linesize[0] as usize + x as usize;
            let lum_y1 = lum_y + out_linesize[0] as usize;
            let chroma = (y >> 1) as usize * out_linesize[1] as usize + x as usize;
            unsafe {
                for k in 0..4usize {
                    *lum.add(lum_y + k) = *input.add(base0 + k * 4 + 1);
                    *lum.add(lum_y1 + k) = *input.add(base1 + k * 4 + 1);
                }
                for pair in 0..2usize {
                    let i0 = base0 + (pair * 2) * 4;
                    let i2 = base1 + (pair * 2) * 4;
                    *chroma_plane.add(chroma + pair * 2) = ((*input.add(i0) as u32
                        + *input.add(i0 + 4) as u32
                        + *input.add(i2) as u32
                        + *input.add(i2 + 4) as u32)
                        >> 2) as u8;
                    *chroma_plane.add(chroma + pair * 2 + 1) = ((*input.add(i0 + 2) as u32
                        + *input.add(i0 + 6) as u32
                        + *input.add(i2 + 2) as u32
                        + *input.add(i2 + 6) as u32)
                        >> 2) as u8;
                }
            }
            x += 4;
        }
        y += 2;
    }
}

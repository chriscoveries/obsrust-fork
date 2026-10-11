//! AVX-512BW tier: 16 px/iter for the hottest path. Bit-exact with the
//! scalar kernel (2x2 chroma = sum-of-4 >> 2, floor).

#![forbid(unsafe_op_in_unsafe_fn)]

#[cfg(target_arch = "x86_64")]
use core::arch::x86_64::*;

#[inline(always)]
fn min_u32(a: u32, b: u32) -> u32 {
    if a < b { a } else { b }
}

/// 16 lum bytes (byte 1 of each u32) from a 64-byte UYVX row.
#[inline(always)]
unsafe fn luma16(row: *const u8) -> u128 {
    unsafe {
        let v = _mm512_loadu_si512(row as *const __m512i);
        let y = _mm512_srli_epi32(_mm512_and_si512(v, _mm512_set1_epi32(0x0000FF00)), 8);
        let p16 = _mm512_packs_epi32(y, y);
        let p8 = _mm512_packus_epi16(p16, p16);
        // each 128-bit lane holds its 4 px bytes at u32 offset 0
        let idx = _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 8, 4, 0);
        let v = _mm512_castsi512_si128(_mm512_permutexvar_epi32(idx, p8));
        let mut out = [0u8; 16];
        _mm_storeu_si128(out.as_mut_ptr() as *mut __m128i, v);
        u128::from_le_bytes(out)
    }
}

/// 2x2 floor-averages for 16 px of one byte channel over two rows.
/// Returns 8 u8s (pairs 0..7).
#[inline(always)]
unsafe fn chroma8(row0: *const u8, row1: *const u8, byte_off: u32) -> u64 {
    unsafe {
        let m = _mm512_set1_epi32(if byte_off == 0 {
            0x000000FF
        } else {
            0x00FF0000
        });
        let sh = byte_off * 8;
        let a = _mm512_srl_epi32(
            _mm512_and_si512(_mm512_loadu_si512(row0 as *const __m512i), m),
            _mm_cvtsi32_si128(sh as i32),
        );
        let b = _mm512_srl_epi32(
            _mm512_and_si512(_mm512_loadu_si512(row1 as *const __m512i), m),
            _mm_cvtsi32_si128(sh as i32),
        );
        let a16 = _mm512_packs_epi32(a, a); // u16 per lane
        let b16 = _mm512_packs_epi32(b, b);
        let a8 = _mm512_packus_epi16(a16, a16); // u8 per lane at u32 off 0
        let b8 = _mm512_packus_epi16(b16, b16);
        // gather each lane's 4 px bytes: [e0, e4, e8, e12] -> 16 u8 px0-15
        let idx = _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 8, 4, 0);
        let r0 = _mm512_castsi512_si128(_mm512_permutexvar_epi32(idx, a8));
        let r1 = _mm512_castsi512_si128(_mm512_permutexvar_epi32(idx, b8));
        // halves: [r0 px0-7, r1 px0-7] and [r0 px8-15, r1 px8-15]
        let lo = _mm_unpacklo_epi64(r0, r1);
        let hi = _mm_unpackhi_epi64(r0, r1);
        let s_lo = _mm_add_epi16(
            _mm_maddubs_epi16(lo, _mm_set1_epi8(1)),
            _mm_srli_si128(_mm_maddubs_epi16(lo, _mm_set1_epi8(1)), 8),
        );
        let s_hi = _mm_add_epi16(
            _mm_maddubs_epi16(hi, _mm_set1_epi8(1)),
            _mm_srli_si128(_mm_maddubs_epi16(hi, _mm_set1_epi8(1)), 8),
        );
        let c0 = _mm_packus_epi16(_mm_srli_epi16(s_lo, 2), _mm_setzero_si128());
        let c1 = _mm_packus_epi16(_mm_srli_epi16(s_hi, 2), _mm_setzero_si128());
        _mm_cvtsi128_si64(_mm_unpacklo_epi32(c0, c1)) as u64
    }
}

/// AVX-512BW UYVX -> I420.
///
/// # Safety
/// Requires avx512bw; caller contract from `compress_uyvx_to_i420`.
#[target_feature(enable = "avx512bw", enable = "avx2")]
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
        while x + 16 <= width {
            let row0 = unsafe { input.add(y as usize * in_linesize as usize + (x as usize) * 4) };
            let row1 = unsafe { row0.add(in_linesize as usize) };
            let lum_y = y as usize * out_linesize[0] as usize + x as usize;
            unsafe {
                std::ptr::write_unaligned(lum.add(lum_y) as *mut u128, luma16(row0));
                std::ptr::write_unaligned(
                    lum.add(lum_y + out_linesize[0] as usize) as *mut u128,
                    luma16(row1),
                );
                let chroma = (y >> 1) as usize * out_linesize[1] as usize + (x >> 1) as usize;
                std::ptr::write_unaligned(u_plane.add(chroma) as *mut u64, chroma8(row0, row1, 0));
                std::ptr::write_unaligned(v_plane.add(chroma) as *mut u64, chroma8(row0, row1, 2));
            }
            x += 16;
        }
        if x < width {
            unsafe {
                super::scalar::compress_uyvx_to_i420_rows(
                    input,
                    in_linesize,
                    y,
                    y + 2,
                    output,
                    out_linesize,
                    x,
                    width,
                )
            };
        }
        y += 2;
    }
}

//! Tier 3 differential test: every kernel + every detected tier must be
//! bit-exact with the C oracle (`libobs/media-io/format-conversion.c`).

use obs_c_oracle as _;
use obs_c_oracle::format_conversion::*;

const W: u32 = 1920;
const H: u32 = 1080;

fn seed(seed: u64) -> impl FnMut() -> u8 {
    let mut s = seed;
    move || {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s as u8
    }
}

struct Planes {
    y: Vec<u8>,
    u: Vec<u8>,
    v: Vec<u8>,
    uv: Vec<u8>,
}

fn planes() -> Planes {
    Planes {
        y: vec![0xCD; (W * H) as usize],
        u: vec![0xCD; (W * H / 4) as usize],
        v: vec![0xCD; (W * H / 4) as usize],
        uv: vec![0xCD; (W * H / 2) as usize],
    }
}

fn uyvx_frame() -> Vec<u8> {
    let mut r = seed(0x1234_5678_9abc_def0);
    (0..W * H * 4).map(|_| r()).collect()
}

#[test]
fn compress_uyvx_to_i420_matches_oracle() {
    let input = uyvx_frame();
    let in_ls = W * 4;
    let ls = [W, W / 2, W / 2];

    let mut oc = planes();
    let mut o = [oc.y.as_mut_ptr(), oc.u.as_mut_ptr(), oc.v.as_mut_ptr()];
    unsafe {
        oracle_compress_uyvx_to_i420(input.as_ptr(), in_ls, 0, H, o.as_mut_ptr(), ls.as_ptr())
    };

    for name in ["scalar", "dispatch"] {
        let mut rs = planes();
        let mut o = [rs.y.as_mut_ptr(), rs.u.as_mut_ptr(), rs.v.as_mut_ptr()];
        unsafe {
            if name == "scalar" {
                obs_convert::scalar::compress_uyvx_to_i420(input.as_ptr(), in_ls, 0, H, &mut o, &ls)
            } else {
                obs_convert::compress_uyvx_to_i420(input.as_ptr(), in_ls, 0, H, &mut o, &ls)
            }
        };
        assert_eq!(rs.y, oc.y, "{name} luma mismatch");
        assert_eq!(rs.u, oc.u, "{name} u mismatch");
        assert_eq!(rs.v, oc.v, "{name} v mismatch");
    }
}

#[test]
fn compress_uyvx_to_nv12_matches_oracle() {
    let input = uyvx_frame();
    let in_ls = W * 4;
    let ls = [W, W];

    let mut oc = planes();
    let mut o = [oc.y.as_mut_ptr(), oc.uv.as_mut_ptr()];
    unsafe {
        oracle_compress_uyvx_to_nv12(input.as_ptr(), in_ls, 0, H, o.as_mut_ptr(), ls.as_ptr())
    };

    for name in ["scalar", "dispatch"] {
        let mut rs = planes();
        let mut o = [rs.y.as_mut_ptr(), rs.uv.as_mut_ptr()];
        unsafe {
            if name == "scalar" {
                obs_convert::scalar::compress_uyvx_to_nv12(input.as_ptr(), in_ls, 0, H, &mut o, &ls)
            } else {
                obs_convert::compress_uyvx_to_nv12(input.as_ptr(), in_ls, 0, H, &mut o, &ls)
            }
        };
        assert_eq!(rs.y, oc.y, "{name} luma mismatch");
        assert_eq!(rs.uv, oc.uv, "{name} uv mismatch");
    }
}

#[test]
fn convert_uyvx_to_i444_matches_oracle() {
    let input = uyvx_frame();
    let in_ls = W * 4;
    let ls = [W, W, W];

    let mut oc = Planes {
        u: vec![0xCD; (W * H) as usize],
        v: vec![0xCD; (W * H) as usize],
        ..planes()
    };
    let mut o = [oc.y.as_mut_ptr(), oc.u.as_mut_ptr(), oc.v.as_mut_ptr()];
    unsafe {
        oracle_convert_uyvx_to_i444(input.as_ptr(), in_ls, 0, H, o.as_mut_ptr(), ls.as_ptr())
    };

    let mut rs = Planes {
        u: vec![0xCD; (W * H) as usize],
        v: vec![0xCD; (W * H) as usize],
        ..planes()
    };
    let mut o = [rs.y.as_mut_ptr(), rs.u.as_mut_ptr(), rs.v.as_mut_ptr()];
    unsafe { obs_convert::convert_uyvx_to_i444(input.as_ptr(), in_ls, 0, H, &mut o, &ls) };

    assert_eq!(rs.y, oc.y);
    assert_eq!(rs.u, oc.u);
    assert_eq!(rs.v, oc.v);
}

#[test]
fn decompress_420_matches_oracle() {
    let mut r = seed(0xfeed_beef_cafe_f00d);
    let y: Vec<u8> = (0..W * H).map(|_| r()).collect();
    let u: Vec<u8> = (0..W * H / 4).map(|_| r()).collect();
    let v: Vec<u8> = (0..W * H / 4).map(|_| r()).collect();
    let ls = [W, W / 2, W / 2];
    let i = [y.as_ptr(), u.as_ptr(), v.as_ptr()];

    let mut oc_out = vec![0u8; (W * H * 4) as usize];
    unsafe { oracle_decompress_420(i.as_ptr(), ls.as_ptr(), 0, H, oc_out.as_mut_ptr(), W * 4) };
    let mut rs_out = vec![0u8; (W * H * 4) as usize];
    unsafe { obs_convert::decompress_420(&i, &ls, 0, H, rs_out.as_mut_ptr(), W * 4) };
    assert_eq!(rs_out, oc_out);
}

#[test]
fn decompress_nv12_matches_oracle() {
    let mut r = seed(0x0bad_c0de_dead_beef);
    let y: Vec<u8> = (0..W * H).map(|_| r()).collect();
    let uv: Vec<u8> = (0..W * H / 2).map(|_| r()).collect();
    let ls = [W, W];
    let i = [y.as_ptr(), uv.as_ptr()];

    let mut oc_out = vec![0u8; (W * H * 4) as usize];
    unsafe { oracle_decompress_nv12(i.as_ptr(), ls.as_ptr(), 0, H, oc_out.as_mut_ptr(), W * 4) };
    let mut rs_out = vec![0u8; (W * H * 4) as usize];
    unsafe { obs_convert::decompress_nv12(&i, &ls, 0, H, rs_out.as_mut_ptr(), W * 4) };
    assert_eq!(rs_out, oc_out);
}

#[test]
fn decompress_422_matches_oracle() {
    let mut r = seed(0xabcd_abcd_abcd_abcd);
    let in_ls = W * 2;
    let out_ls = W * 4;
    // The kernel reads min(in,out)/2 u32s per row — 2*in_linesize bytes when
    // in_linesize < out_linesize — so the last row reads a full line past
    // the nominal input end; the same overlap applies on the output side.
    let input: Vec<u8> = (0..(H + 1) * in_ls).map(|_| r()).collect();
    let out_cap = ((H - 1) * out_ls + in_ls * 4) as usize;

    for leading in [true, false] {
        let mut oc_out = vec![0u8; out_cap];
        unsafe {
            oracle_decompress_422(
                input.as_ptr(),
                in_ls,
                0,
                H,
                oc_out.as_mut_ptr(),
                out_ls,
                leading,
            )
        };
        let mut rs_out = vec![0u8; out_cap];
        unsafe {
            obs_convert::decompress_422(
                input.as_ptr(),
                in_ls,
                0,
                H,
                rs_out.as_mut_ptr(),
                out_ls,
                leading,
            )
        };
        assert_eq!(rs_out, oc_out, "leading_lum={leading}");
    }
}

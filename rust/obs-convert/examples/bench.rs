//! 1080p ns/frame microbench for the conversion kernels (perf P3).
//! Run: soldr cargo run -p obs-convert --release --example bench

use std::time::Instant;

use obs_c_oracle as _;

const W: u32 = 1920;
const H: u32 = 1080;
const ITERS: usize = 200;

fn main() {
    let mut s = 0x1234_5678_9abc_def0u64;
    let mut r = move || {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s as u8
    };
    let input: Vec<u8> = (0..W * H * 4).map(|_| r()).collect();
    let in_ls = W * 4;
    let ls420 = [W, W / 2, W / 2];
    let lsnv12 = [W, W];
    let ls444 = [W, W, W];

    fn times<F: FnMut()>(name: &str, mut f: F) {
        let mut v = Vec::with_capacity(ITERS);
        for _ in 0..ITERS {
            let t = Instant::now();
            f();
            v.push(t.elapsed().as_nanos() as u64);
        }
        v.sort_unstable();
        println!(
            "{name:34} median {:8} ns/frame  (min {:8})",
            v[ITERS / 2],
            v[0]
        );
    }

    let mut y = vec![0u8; (W * H) as usize];
    let mut u = vec![0u8; (W * H / 4) as usize];
    let mut v = vec![0u8; (W * H / 4) as usize];
    let mut uv = vec![0u8; (W * H / 2) as usize];
    let mut u4 = vec![0u8; (W * H) as usize];
    let mut v4 = vec![0u8; (W * H) as usize];
    let mut packed = vec![0u8; (W * H * 4) as usize];

    times("scalar uyvx->i420", || unsafe {
        let mut o = [y.as_mut_ptr(), u.as_mut_ptr(), v.as_mut_ptr()];
        obs_convert::scalar::compress_uyvx_to_i420(input.as_ptr(), in_ls, 0, H, &mut o, &ls420)
    });
    times("dispatch uyvx->i420", || unsafe {
        let mut o = [y.as_mut_ptr(), u.as_mut_ptr(), v.as_mut_ptr()];
        obs_convert::compress_uyvx_to_i420(input.as_ptr(), in_ls, 0, H, &mut o, &ls420)
    });
    times("scalar uyvx->nv12", || unsafe {
        let mut o = [y.as_mut_ptr(), uv.as_mut_ptr()];
        obs_convert::scalar::compress_uyvx_to_nv12(input.as_ptr(), in_ls, 0, H, &mut o, &lsnv12)
    });
    times("dispatch uyvx->nv12", || unsafe {
        let mut o = [y.as_mut_ptr(), uv.as_mut_ptr()];
        obs_convert::compress_uyvx_to_nv12(input.as_ptr(), in_ls, 0, H, &mut o, &lsnv12)
    });
    times("dispatch uyvx->i444", || unsafe {
        let mut o = [y.as_mut_ptr(), u4.as_mut_ptr(), v4.as_mut_ptr()];
        obs_convert::convert_uyvx_to_i444(input.as_ptr(), in_ls, 0, H, &mut o, &ls444)
    });
    times("dispatch i420->packed", || unsafe {
        let i = [y.as_ptr(), u.as_ptr(), v.as_ptr()];
        obs_convert::decompress_420(&i, &ls420, 0, H, packed.as_mut_ptr(), W * 4)
    });
    times("dispatch nv12->packed", || unsafe {
        let i = [y.as_ptr(), uv.as_ptr()];
        obs_convert::decompress_nv12(&i, &lsnv12, 0, H, packed.as_mut_ptr(), W * 4)
    });
    times("dispatch decomp422", || unsafe {
        obs_convert::decompress_422(
            input.as_ptr(),
            W * 2,
            0,
            H,
            packed.as_mut_ptr(),
            W * 4,
            false,
        )
    });
}

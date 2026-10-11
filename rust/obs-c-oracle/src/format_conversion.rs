//! Independent declarations of the `oracle_*` conversion kernels
//! (`oracle/format_conversion.c`). Parameterized by row ranges exactly like
//! the C kernels.

unsafe extern "C" {
    pub fn oracle_compress_uyvx_to_i420(
        input: *const u8,
        in_linesize: u32,
        start_y: u32,
        end_y: u32,
        output: *const *mut u8,
        out_linesize: *const u32,
    );
    pub fn oracle_compress_uyvx_to_nv12(
        input: *const u8,
        in_linesize: u32,
        start_y: u32,
        end_y: u32,
        output: *const *mut u8,
        out_linesize: *const u32,
    );
    pub fn oracle_convert_uyvx_to_i444(
        input: *const u8,
        in_linesize: u32,
        start_y: u32,
        end_y: u32,
        output: *const *mut u8,
        out_linesize: *const u32,
    );
    pub fn oracle_decompress_420(
        input: *const *const u8,
        in_linesize: *const u32,
        start_y: u32,
        end_y: u32,
        output: *mut u8,
        out_linesize: u32,
    );
    pub fn oracle_decompress_nv12(
        input: *const *const u8,
        in_linesize: *const u32,
        start_y: u32,
        end_y: u32,
        output: *mut u8,
        out_linesize: u32,
    );
    pub fn oracle_decompress_422(
        input: *const u8,
        in_linesize: u32,
        start_y: u32,
        end_y: u32,
        output: *mut u8,
        out_linesize: u32,
        leading_lum: bool,
    );
}

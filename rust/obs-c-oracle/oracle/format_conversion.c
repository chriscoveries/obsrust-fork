/* Test-only oracle: the original libobs/media-io/format-conversion.c,
 * unmodified, with its global symbols renamed to oracle_* so it can link
 * next to the Rust implementation. */
#include <stddef.h>

#define compress_uyvx_to_i420 oracle_compress_uyvx_to_i420
#define compress_uyvx_to_nv12 oracle_compress_uyvx_to_nv12
#define convert_uyvx_to_i444 oracle_convert_uyvx_to_i444
#define decompress_420 oracle_decompress_420
#define decompress_nv12 oracle_decompress_nv12
#define decompress_422 oracle_decompress_422

#include "media-io/format-conversion.c"

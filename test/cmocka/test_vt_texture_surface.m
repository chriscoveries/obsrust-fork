/* Windowless plane-copy parity check for the VideoToolbox IOSurface bridge.
 * Production fallback dispatch is exercised by test_vt_texture_raw_fallback. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include "vt-frame-copy.h"

#define WIDTH 1920
#define HEIGHT 1080

static void fill_plane(uint8_t *plane, size_t row_bytes, size_t height, uint8_t seed)
{
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < row_bytes; x++)
            plane[y * row_bytes + x] = (uint8_t) (seed + x * 17 + y * 29);
    }
}

static void assert_planes_equal(CVPixelBufferRef expected, CVPixelBufferRef actual, size_t active_row_bytes)
{
    assert_int_equal(CVPixelBufferGetPlaneCount(expected), 2);
    assert_int_equal(CVPixelBufferGetPlaneCount(actual), 2);

    for (size_t plane = 0; plane < 2; plane++) {
        assert_int_equal(CVPixelBufferGetHeightOfPlane(expected, plane), CVPixelBufferGetHeightOfPlane(actual, plane));
        uint8_t *expected_row = CVPixelBufferGetBaseAddressOfPlane(expected, plane);
        uint8_t *actual_row = CVPixelBufferGetBaseAddressOfPlane(actual, plane);
        size_t expected_stride = CVPixelBufferGetBytesPerRowOfPlane(expected, plane);
        size_t actual_stride = CVPixelBufferGetBytesPerRowOfPlane(actual, plane);
        size_t height = CVPixelBufferGetHeightOfPlane(expected, plane);
        assert_non_null(expected_row);
        assert_non_null(actual_row);
        assert_true(expected_stride >= active_row_bytes);
        assert_true(actual_stride >= active_row_bytes);

        for (size_t row = 0; row < height; row++) {
            assert_memory_equal(expected_row, actual_row, active_row_bytes);
            expected_row += expected_stride;
            actual_row += actual_stride;
        }
    }
}

struct encoded_result {
    CMSampleBufferRef sample_buffer;
    OSStatus status;
};

static void vt_output_callback(void *output_refcon, void *source_refcon, OSStatus status, VTEncodeInfoFlags info_flags,
                               CMSampleBufferRef sample_buffer)
{
    (void) source_refcon;
    (void) info_flags;
    struct encoded_result *result = output_refcon;
    if (status != noErr || !sample_buffer) {
        result->status = status == noErr ? -1 : status;
        return;
    }
    CFRetain(sample_buffer);
    result->sample_buffer = sample_buffer;
}

static CMSampleBufferRef encode_single_frame(CVPixelBufferRef pixel_buffer)
{
    struct encoded_result result = {0};

    VTCompressionSessionRef session = NULL;
    assert_int_equal(VTCompressionSessionCreate(kCFAllocatorDefault, WIDTH, HEIGHT, kCMVideoCodecType_H264, NULL, NULL,
                                                NULL, vt_output_callback, &result, &session),
                     noErr);
    assert_non_null(session);
    assert_int_equal(VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue), noErr);
    assert_int_equal(VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse),
                     noErr);
    assert_int_equal(VTCompressionSessionPrepareToEncodeFrames(session), noErr);
    assert_int_equal(VTCompressionSessionEncodeFrame(session, pixel_buffer, CMTimeMake(0, 30), CMTimeMake(1, 30), NULL,
                                                     NULL, NULL),
                     noErr);
    assert_int_equal(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid), noErr);
    assert_int_equal(result.status, noErr);
    assert_non_null(result.sample_buffer);

    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return result.sample_buffer;
}

struct decoded_result {
    CVPixelBufferRef pixel_buffer;
    OSStatus status;
};

static void vt_decompression_output_callback(void *output_refcon, void *source_refcon, OSStatus status,
                                             VTDecodeInfoFlags info_flags, CVImageBufferRef image_buffer, CMTime pts,
                                             CMTime duration)
{
    (void) source_refcon;
    (void) info_flags;
    (void) pts;
    (void) duration;
    struct decoded_result *result = output_refcon;
    if (status != noErr || !image_buffer) {
        result->status = status == noErr ? -1 : status;
        return;
    }
    CFRetain(image_buffer);
    result->pixel_buffer = (CVPixelBufferRef) image_buffer;
}

static CVPixelBufferRef decode_single_frame(CMSampleBufferRef sample_buffer)
{
    struct decoded_result result = {0};
    VTDecompressionOutputCallbackRecord callback = {
        .decompressionOutputCallback = vt_decompression_output_callback,
        .decompressionOutputRefCon = &result,
    };
    VTDecompressionSessionRef session = NULL;
    assert_int_equal(VTDecompressionSessionCreate(kCFAllocatorDefault,
                                                  CMSampleBufferGetFormatDescription(sample_buffer), NULL, NULL,
                                                  &callback, &session),
                     noErr);
    assert_non_null(session);
    VTDecodeInfoFlags info_flags = 0;
    assert_int_equal(VTDecompressionSessionDecodeFrame(
                         session, sample_buffer, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &info_flags),
                     noErr);
    assert_int_equal(VTDecompressionSessionWaitForAsynchronousFrames(session), noErr);
    assert_int_equal(result.status, noErr);
    assert_non_null(result.pixel_buffer);

    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    return result.pixel_buffer;
}

static void exercise_surface_format(id<MTLDevice> device, OSType pixel_format, MTLPixelFormat luma_format,
                                    MTLPixelFormat chroma_format, size_t bytes_per_component)
{
    NSDictionary *pixel_buffer_attributes = @{
        (NSString *) kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString *) kCVPixelBufferIOSurfacePropertiesKey: @ {},
    };
    CVPixelBufferRef surface_buffer = NULL;
    assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, WIDTH, HEIGHT, pixel_format,
                                         (__bridge CFDictionaryRef) pixel_buffer_attributes, &surface_buffer),
                     (CVReturn) kCVReturnSuccess);
    assert_non_null(surface_buffer);
    assert_int_equal(CVPixelBufferLockBaseAddress(surface_buffer, 0), kCVReturnSuccess);

    size_t active_row_bytes = WIDTH * bytes_per_component;
    uint8_t *y_data = malloc(active_row_bytes * HEIGHT);
    uint8_t *uv_data = malloc(active_row_bytes * HEIGHT / 2);
    assert_non_null(y_data);
    assert_non_null(uv_data);
    fill_plane(y_data, active_row_bytes, HEIGHT, 3);
    fill_plane(uv_data, active_row_bytes, HEIGHT / 2, 101);

    uint8_t *surface_y = CVPixelBufferGetBaseAddressOfPlane(surface_buffer, 0);
    uint8_t *surface_uv = CVPixelBufferGetBaseAddressOfPlane(surface_buffer, 1);
    size_t surface_y_stride = CVPixelBufferGetBytesPerRowOfPlane(surface_buffer, 0);
    size_t surface_uv_stride = CVPixelBufferGetBytesPerRowOfPlane(surface_buffer, 1);
    assert_non_null(surface_y);
    assert_non_null(surface_uv);
    assert_true(surface_y_stride >= active_row_bytes);
    assert_true(surface_uv_stride >= active_row_bytes);
    for (size_t row = 0; row < HEIGHT; row++)
        memcpy(surface_y + row * surface_y_stride, y_data + row * active_row_bytes, active_row_bytes);
    for (size_t row = 0; row < HEIGHT / 2; row++)
        memcpy(surface_uv + row * surface_uv_stride, uv_data + row * active_row_bytes, active_row_bytes);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(surface_buffer, 0), kCVReturnSuccess);

    CVMetalTextureCacheRef texture_cache = NULL;
    assert_int_equal(CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, device, NULL, &texture_cache),
                     kCVReturnSuccess);
    assert_non_null(texture_cache);
    NSDictionary *texture_attributes = @{
        (NSString *) kCVMetalTextureUsage: @(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget),
    };
    CVMetalTextureRef luma_view = NULL;
    CVMetalTextureRef chroma_view = NULL;
    assert_int_equal(CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texture_cache, surface_buffer,
                                                               (__bridge CFDictionaryRef) texture_attributes,
                                                               luma_format, WIDTH, HEIGHT, 0, &luma_view),
                     kCVReturnSuccess);
    assert_int_equal(CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texture_cache, surface_buffer,
                                                               (__bridge CFDictionaryRef) texture_attributes,
                                                               chroma_format, WIDTH / 2, HEIGHT / 2, 1, &chroma_view),
                     kCVReturnSuccess);
    assert_non_null(CVMetalTextureGetTexture(luma_view));
    assert_non_null(CVMetalTextureGetTexture(chroma_view));

    IOSurfaceRef surface = CVPixelBufferGetIOSurface(surface_buffer);
    assert_non_null(surface);
    CVPixelBufferRef texture_path_buffer = NULL;
    assert_int_equal(CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, NULL, &texture_path_buffer),
                     kCVReturnSuccess);
    assert_non_null(texture_path_buffer);
    assert_int_equal(IOSurfaceGetID(CVPixelBufferGetIOSurface(texture_path_buffer)), IOSurfaceGetID(surface));
    assert_int_equal(CVPixelBufferGetPixelFormatType(texture_path_buffer), pixel_format);

    CVPixelBufferRef raw_path_buffer = NULL;
    assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, WIDTH, HEIGHT, pixel_format, NULL, &raw_path_buffer),
                     kCVReturnSuccess);
    assert_non_null(raw_path_buffer);
    assert_int_equal(CVPixelBufferLockBaseAddress(raw_path_buffer, 0), kCVReturnSuccess);
    uint8_t *frame_data[MAX_AV_PLANES] = {y_data, uv_data, NULL};
    uint32_t linesizes[MAX_AV_PLANES] = {(uint32_t) active_row_bytes, (uint32_t) active_row_bytes, 0};
    size_t raw_path_bytes_copied = vt_copy_frame_to_pixbuf(raw_path_buffer, frame_data, linesizes);
    assert_int_equal(raw_path_bytes_copied, active_row_bytes * HEIGHT * 3 / 2);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(raw_path_buffer, 0), kCVReturnSuccess);

    /* Characterize plane-copy output when an IOSurface-backed pixel buffer is
     * copied into a pool-style destination buffer. */
    CVPixelBufferRef fallback_path_buffer = NULL;
    assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, WIDTH, HEIGHT, pixel_format, NULL, &fallback_path_buffer),
                     kCVReturnSuccess);
    assert_non_null(fallback_path_buffer);
    assert_int_equal(CVPixelBufferLockBaseAddress(texture_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_int_equal(CVPixelBufferLockBaseAddress(fallback_path_buffer, 0), kCVReturnSuccess);
    uint8_t *surface_frame_data[MAX_AV_PLANES] = {
        CVPixelBufferGetBaseAddressOfPlane(texture_path_buffer, 0),
        CVPixelBufferGetBaseAddressOfPlane(texture_path_buffer, 1),
        NULL,
    };
    uint32_t surface_linesizes[MAX_AV_PLANES] = {
        (uint32_t) CVPixelBufferGetBytesPerRowOfPlane(texture_path_buffer, 0),
        (uint32_t) CVPixelBufferGetBytesPerRowOfPlane(texture_path_buffer, 1),
        0,
    };
    assert_non_null(surface_frame_data[0]);
    assert_non_null(surface_frame_data[1]);
    assert_true(surface_linesizes[0] <= CVPixelBufferGetBytesPerRowOfPlane(fallback_path_buffer, 0));
    assert_true(surface_linesizes[1] <= CVPixelBufferGetBytesPerRowOfPlane(fallback_path_buffer, 1));
    vt_copy_bytes_reset();
    size_t fallback_bytes_copied = vt_copy_frame_to_pixbuf(fallback_path_buffer, surface_frame_data, surface_linesizes);
    assert_int_equal(fallback_bytes_copied, active_row_bytes * HEIGHT * 3 / 2);
    assert_int_equal(vt_copy_bytes_total(), fallback_bytes_copied);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(fallback_path_buffer, 0), kCVReturnSuccess);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(texture_path_buffer, kCVPixelBufferLock_ReadOnly),
                     kCVReturnSuccess);

    /* Characterize the same plane copy from IOSurface base addresses. */
    CVPixelBufferRef surface_fallback_buffer = NULL;
    assert_int_equal(CVPixelBufferCreate(kCFAllocatorDefault, WIDTH, HEIGHT, pixel_format, NULL,
                                         &surface_fallback_buffer),
                     kCVReturnSuccess);
    assert_non_null(surface_fallback_buffer);
    assert_int_equal(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL), kIOReturnSuccess);
    assert_int_equal(CVPixelBufferLockBaseAddress(surface_fallback_buffer, 0), kCVReturnSuccess);
    uint8_t *iosurface_data[MAX_AV_PLANES] = {
        IOSurfaceGetBaseAddressOfPlane(surface, 0),
        IOSurfaceGetBaseAddressOfPlane(surface, 1),
        NULL,
    };
    uint32_t iosurface_linesizes[MAX_AV_PLANES] = {
        (uint32_t) IOSurfaceGetBytesPerRowOfPlane(surface, 0),
        (uint32_t) IOSurfaceGetBytesPerRowOfPlane(surface, 1),
        0,
    };
    size_t iosurface_expected_bytes = 0;
    for (size_t plane = 0; plane < 2; plane++) {
        assert_non_null(iosurface_data[plane]);
        assert_true(iosurface_linesizes[plane] <= CVPixelBufferGetBytesPerRowOfPlane(surface_fallback_buffer, plane));
        assert_int_equal(IOSurfaceGetHeightOfPlane(surface, plane),
                         CVPixelBufferGetHeightOfPlane(surface_fallback_buffer, plane));
        iosurface_expected_bytes += iosurface_linesizes[plane] * IOSurfaceGetHeightOfPlane(surface, plane);
    }
    vt_copy_bytes_reset();
    size_t surface_fallback_bytes =
        vt_copy_frame_to_pixbuf(surface_fallback_buffer, iosurface_data, iosurface_linesizes);
    assert_int_equal(surface_fallback_bytes, iosurface_expected_bytes);
    assert_int_equal(vt_copy_bytes_total(), surface_fallback_bytes);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(surface_fallback_buffer, 0), kCVReturnSuccess);
    assert_int_equal(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL), kIOReturnSuccess);

    /* The texture route wraps the same IOSurface without invoking the copy seam. */
    assert_int_equal(CVPixelBufferLockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_int_equal(CVPixelBufferLockBaseAddress(texture_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_planes_equal(raw_path_buffer, texture_path_buffer, active_row_bytes);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(texture_path_buffer, kCVPixelBufferLock_ReadOnly),
                     kCVReturnSuccess);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);

    assert_int_equal(CVPixelBufferLockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_int_equal(CVPixelBufferLockBaseAddress(fallback_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_planes_equal(raw_path_buffer, fallback_path_buffer, active_row_bytes);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(fallback_path_buffer, kCVPixelBufferLock_ReadOnly),
                     kCVReturnSuccess);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);

    assert_int_equal(CVPixelBufferLockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    assert_int_equal(CVPixelBufferLockBaseAddress(surface_fallback_buffer, kCVPixelBufferLock_ReadOnly),
                     kCVReturnSuccess);
    assert_planes_equal(raw_path_buffer, surface_fallback_buffer, active_row_bytes);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(surface_fallback_buffer, kCVPixelBufferLock_ReadOnly),
                     kCVReturnSuccess);
    assert_int_equal(CVPixelBufferUnlockBaseAddress(raw_path_buffer, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
    if (pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        CMSampleBufferRef raw_encoded = encode_single_frame(raw_path_buffer);
        CMSampleBufferRef texture_encoded = encode_single_frame(texture_path_buffer);
        CMSampleBufferRef fallback_encoded = encode_single_frame(fallback_path_buffer);
        CMSampleBufferRef surface_fallback_encoded = encode_single_frame(surface_fallback_buffer);
        CVPixelBufferRef raw_decoded = decode_single_frame(raw_encoded);
        CVPixelBufferRef texture_decoded = decode_single_frame(texture_encoded);
        CVPixelBufferRef fallback_decoded = decode_single_frame(fallback_encoded);
        CVPixelBufferRef surface_fallback_decoded = decode_single_frame(surface_fallback_encoded);
        assert_int_equal(CVPixelBufferLockBaseAddress(raw_decoded, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
        assert_int_equal(CVPixelBufferLockBaseAddress(texture_decoded, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
        assert_int_equal(CVPixelBufferLockBaseAddress(fallback_decoded, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
        assert_int_equal(CVPixelBufferLockBaseAddress(surface_fallback_decoded, kCVPixelBufferLock_ReadOnly),
                         kCVReturnSuccess);
        assert_planes_equal(raw_decoded, texture_decoded, WIDTH);
        assert_planes_equal(raw_decoded, fallback_decoded, WIDTH);
        assert_planes_equal(raw_decoded, surface_fallback_decoded, WIDTH);
        assert_int_equal(CVPixelBufferUnlockBaseAddress(surface_fallback_decoded, kCVPixelBufferLock_ReadOnly),
                         kCVReturnSuccess);
        assert_int_equal(CVPixelBufferUnlockBaseAddress(fallback_decoded, kCVPixelBufferLock_ReadOnly),
                         kCVReturnSuccess);
        assert_int_equal(CVPixelBufferUnlockBaseAddress(texture_decoded, kCVPixelBufferLock_ReadOnly),
                         kCVReturnSuccess);
        assert_int_equal(CVPixelBufferUnlockBaseAddress(raw_decoded, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
        CVPixelBufferRelease(raw_decoded);
        CVPixelBufferRelease(texture_decoded);
        CVPixelBufferRelease(fallback_decoded);
        CVPixelBufferRelease(surface_fallback_decoded);
        CFRelease(raw_encoded);
        CFRelease(texture_encoded);
        CFRelease(fallback_encoded);
        CFRelease(surface_fallback_encoded);
    }

    CVPixelBufferRelease(raw_path_buffer);
    CVPixelBufferRelease(fallback_path_buffer);
    CVPixelBufferRelease(surface_fallback_buffer);
    CVPixelBufferRelease(texture_path_buffer);
    CFRelease(luma_view);
    CFRelease(chroma_view);
    CVMetalTextureCacheFlush(texture_cache, 0);
    CFRelease(texture_cache);
    CVPixelBufferRelease(surface_buffer);
    free(y_data);
    free(uv_data);
}

static void test_nv12_and_p010_iosurface_planes(void **state)
{
    (void) state;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    assert_non_null(device);

    exercise_surface_format(device, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, MTLPixelFormatR8Unorm,
                            MTLPixelFormatRG8Unorm, 1);
    exercise_surface_format(device, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, MTLPixelFormatR8Unorm,
                            MTLPixelFormatRG8Unorm, 1);
    exercise_surface_format(device, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, MTLPixelFormatR16Unorm,
                            MTLPixelFormatRG16Unorm, 2);
    exercise_surface_format(device, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, MTLPixelFormatR16Unorm,
                            MTLPixelFormatRG16Unorm, 2);
}

int main(void)
{
    @autoreleasepool {
        const struct CMUnitTest tests[] = {cmocka_unit_test(test_nv12_and_p010_iosurface_planes)};
        return cmocka_run_group_tests(tests, NULL, NULL);
    }
}

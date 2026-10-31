/******************************************************************************
 Copyright (C) 2026 by the OBS Studio contributors

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

import CoreVideo
import Foundation
import Metal

private func createVideoTexturePair(
    device: MetalDevice, width: UInt32, height: UInt32, fullRange: Bool, tenBit: Bool
) -> (CVPixelBuffer, MetalTexture, MetalTexture)? {
    guard let cache = device.videoTextureCache else { return nil }

    let pixelFormat: OSType
    let lumaFormat: MTLPixelFormat
    let chromaFormat: MTLPixelFormat
    if tenBit {
        pixelFormat = fullRange ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
            : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        lumaFormat = .r16Unorm
        chromaFormat = .rg16Unorm
    } else {
        pixelFormat = fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        lumaFormat = .r8Unorm
        chromaFormat = .rg8Unorm
    }

    let pixelBufferAttributes: [String: Any] = [
        kCVPixelBufferMetalCompatibilityKey as String: true,
        kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
    ]

    var pixelBuffer: CVPixelBuffer?
    guard
        CVPixelBufferCreate(
            kCFAllocatorDefault,
            Int(width),
            Int(height),
            pixelFormat,
            pixelBufferAttributes as CFDictionary,
            &pixelBuffer
        ) == kCVReturnSuccess,
        let pixelBuffer
    else {
        return nil
    }

    let textureAttributes: [CFString: Any] = [
        kCVMetalTextureUsage: NSNumber(value: MTLTextureUsage.shaderRead.union(.renderTarget).rawValue)
    ]

    var cvLuma: CVMetalTexture?
    var cvChroma: CVMetalTexture?
    guard
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            cache,
            pixelBuffer,
            textureAttributes as CFDictionary,
            lumaFormat,
            Int(width),
            Int(height),
            0,
            &cvLuma
        ) == kCVReturnSuccess,
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            cache,
            pixelBuffer,
            textureAttributes as CFDictionary,
            chromaFormat,
            Int(width / 2),
            Int(height / 2),
            1,
            &cvChroma
        ) == kCVReturnSuccess,
        let cvLuma,
        let cvChroma,
        let luma = CVMetalTextureGetTexture(cvLuma),
        let chroma = CVMetalTextureGetTexture(cvChroma),
        let lumaTexture = MetalTexture(device: device, texture: luma),
        let chromaTexture = MetalTexture(device: device, texture: chroma)
    else {
        return nil
    }

    lumaTexture.videoPixelBuffer = pixelBuffer
    lumaTexture.cvMetalTexture = cvLuma
    chromaTexture.videoPixelBuffer = pixelBuffer
    chromaTexture.cvMetalTexture = cvChroma

    return (pixelBuffer, lumaTexture, chromaTexture)
}

@_cdecl("device_nv12_available")
public func device_nv12_available(device: UnsafeRawPointer) -> Bool {
    let device: MetalDevice = unretained(device)
    return device.videoTextureCache != nil
}

@_cdecl("device_p010_available")
public func device_p010_available(device: UnsafeRawPointer) -> Bool {
    let device: MetalDevice = unretained(device)
    return device.videoTextureCache != nil
}

@_cdecl("device_texture_create_nv12")
public func device_texture_create_nv12(
    device: UnsafeRawPointer,
    texY: UnsafeMutablePointer<OpaquePointer?>,
    texUV: UnsafeMutablePointer<OpaquePointer?>,
    width: UInt32,
    height: UInt32,
    flags: UInt32
) -> Bool {
    let device: MetalDevice = unretained(device)
    texY.pointee = nil
    texUV.pointee = nil

    guard width.isMultiple(of: 2), height.isMultiple(of: 2),
        let (_, luma, chroma) = createVideoTexturePair(
            device: device, width: width, height: height, fullRange: (flags & UInt32(GS_FULL_RANGE_TEX)) != 0,
            tenBit: false
        )
    else {
        return false
    }

    texY.pointee = luma.getRetained()
    texUV.pointee = chroma.getRetained()
    return true
}

@_cdecl("device_texture_create_p010")
public func device_texture_create_p010(
    device: UnsafeRawPointer,
    texY: UnsafeMutablePointer<OpaquePointer?>,
    texUV: UnsafeMutablePointer<OpaquePointer?>,
    width: UInt32,
    height: UInt32,
    flags: UInt32
) -> Bool {
    let device: MetalDevice = unretained(device)
    texY.pointee = nil
    texUV.pointee = nil

    guard width.isMultiple(of: 2), height.isMultiple(of: 2),
        let (_, luma, chroma) = createVideoTexturePair(
            device: device, width: width, height: height, fullRange: (flags & UInt32(GS_FULL_RANGE_TEX)) != 0,
            tenBit: true
        )
    else {
        return false
    }

    texY.pointee = luma.getRetained()
    texUV.pointee = chroma.getRetained()
    return true
}

@_cdecl("gs_texture_get_iosurface")
public func gs_texture_get_iosurface(tex: UnsafeRawPointer) -> UnsafeMutableRawPointer? {
    let texture: MetalTexture = unretained(tex)
    guard let pixelBuffer = texture.videoPixelBuffer,
        let surface = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue()
    else {
        return nil
    }

    return Unmanaged.passUnretained(surface).toOpaque()
}

@_cdecl("device_texture_wait_for_completion")
public func device_texture_wait_for_completion(
    device: UnsafeRawPointer,
    tex: UnsafeRawPointer
) -> Bool {
    let device: MetalDevice = unretained(device)
    let texture: MetalTexture = unretained(tex)

    guard texture.device === device, let commandBuffer = device.commandQueue.makeCommandBuffer() else {
        return false
    }

    commandBuffer.commit()
    commandBuffer.waitUntilCompleted()
    return commandBuffer.status == .completed
}

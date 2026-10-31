import Darwin
import Metal

@main
enum MetalTextureCompletionTest {
    static func fail(_ message: String) -> Never {
        fputs("test_metal_texture_completion: \(message)\n", stderr)
        exit(1)
    }

    static func main() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
            let commandBuffer = queue.makeCommandBuffer()
        else {
            exit(77)
        }

        let completion = MetalTextureCompletion()
        guard completion.waitUntilCompleted() else {
            fail("an unused completion tracker should already be complete")
        }

        completion.record(commandBuffer)
        commandBuffer.commit()
        guard completion.waitUntilCompleted(), commandBuffer.status == .completed else {
            fail("the recorded command buffer did not complete")
        }
        guard completion.waitUntilCompleted() else {
            fail("a completed command buffer should be consumed")
        }
    }
}

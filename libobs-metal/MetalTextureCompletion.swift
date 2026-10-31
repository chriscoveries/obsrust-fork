/******************************************************************************
 Copyright (C) 2026 by Chriscoveries <Chriscoveries@gmail.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program. If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

import Foundation
import Metal

final class MetalTextureCompletion {
    private let lock = NSLock()
    private var commandBuffer: MTLCommandBuffer?

    func record(_ commandBuffer: MTLCommandBuffer) {
        lock.lock()
        self.commandBuffer = commandBuffer
        lock.unlock()
    }

    func waitUntilCompleted() -> Bool {
        lock.lock()
        let commandBuffer = self.commandBuffer
        self.commandBuffer = nil
        lock.unlock()

        guard let commandBuffer else {
            return true
        }

        commandBuffer.waitUntilCompleted()
        return commandBuffer.status == .completed
    }
}

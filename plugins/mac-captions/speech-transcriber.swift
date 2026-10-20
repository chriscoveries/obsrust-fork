import AVFoundation
import CoreMedia
import Foundation
import Speech

#if MAC_CAPTIONS_ASSET_INSTALLER
    import Darwin
#endif

public typealias MacCaptionsWordCallback =
    @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<CChar>?, Double, Double
    ) -> Void

private enum ShimError: Int32, Error {
    case ok = 0
    case badArguments = -1
    case unavailable = -2
    case localeUnavailable = -3
    case assetsMissing = -4
    case formatUnavailable = -5
    case analyzerFailed = -6
    case reservationFailed = -7
    case installationRequestUnavailable = -8
}

private final class BlockingResult<Value>: @unchecked Sendable {
    private let semaphore = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var result: Result<Value, Error>?

    func finish(_ result: Result<Value, Error>) {
        lock.lock()
        self.result = result
        lock.unlock()
        semaphore.signal()
    }

    func wait() throws -> Value {
        semaphore.wait()
        lock.lock()
        let result = self.result!
        lock.unlock()
        return try result.get()
    }
}

private func runBlocking<Value>(_ operation: @escaping @Sendable () async throws -> Value) throws -> Value {
    let result = BlockingResult<Value>()
    Task {
        do {
            result.finish(.success(try await operation()))
        } catch {
            result.finish(.failure(error))
        }
    }
    return try result.wait()
}

@available(macOS 27.0, *)
private func makeTranscriber() async throws -> (SpeechTranscriber, [any SpeechModule]) {
    guard SpeechTranscriber.isAvailable else { throw ShimError.unavailable }
    guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: "en-US"))
    else { throw ShimError.localeUnavailable }
    let transcriber = SpeechTranscriber(
        locale: locale,
        transcriptionOptions: [],
        reportingOptions: [.volatileResults, .fastResults],
        attributeOptions: [.audioTimeRange]
    )
    return (transcriber, [transcriber])
}

#if MAC_CAPTIONS_ASSET_INSTALLER
    @available(macOS 27.0, *)
    private func installEnglishSpeechAssets() async throws {
        let (transcriber, modules) = try await makeTranscriber()
        guard let locale = transcriber.selectedLocales.first else { throw ShimError.localeUnavailable }
        let before = await AssetInventory.status(forModules: modules)
        print("status_before=\(String(describing: before))")
        print("maximum_reserved_locales=\(AssetInventory.maximumReservedLocales)")
        let reserved = await AssetInventory.reservedLocales
        print("reserved_locales_before=\(reserved.map(\.identifier).joined(separator: ","))")

        guard before != .unsupported else {
            throw ShimError.unavailable
        }
        if before == .installed {
            print("asset_already_installed=true")
            print("installation_request=not_called")
            print("status_after=\(String(describing: await AssetInventory.status(forModules: modules)))")
            return
        }

        do {
            if reserved.contains(where: { $0.identifier == locale.identifier }) {
                print("reserve_result=already_reserved")
            } else {
                let didReserve = try await AssetInventory.reserve(locale: locale)
                print("reserve_result=\(didReserve)")
                guard didReserve else { throw ShimError.reservationFailed }
            }

            let request = try await AssetInventory.assetInstallationRequest(supporting: modules)
            guard let request else {
                print("installation_request=nil")
                let after = await AssetInventory.status(forModules: modules)
                print("status_after=\(String(describing: after))")
                guard after == .installed else { throw ShimError.installationRequestUnavailable }
                return
            }

            print("installation_request=non_nil")
            try await request.downloadAndInstall()
        } catch {
            let after = await AssetInventory.status(forModules: modules)
            print("status_after=\(String(describing: after))")
            throw error
        }

        let after = await AssetInventory.status(forModules: modules)
        print("status_after=\(String(describing: after))")
        guard after == .installed else { throw ShimError.analyzerFailed }
    }
#endif

@available(macOS 27.0, *)
private func currentAssetStatus() async throws -> AssetInventory.Status {
    let (transcriber, modules) = try await makeTranscriber()
    let before = await AssetInventory.status(forModules: modules)
    guard before == .supported, let locale = transcriber.selectedLocales.first else { return before }
    let reserved = await AssetInventory.reservedLocales
    if reserved.contains(where: { $0.identifier == locale.identifier }) { return before }
    guard try await AssetInventory.reserve(locale: locale) else { return before }
    let after = await AssetInventory.status(forModules: modules)
    _ = await AssetInventory.release(reservedLocale: locale)
    return after
}

private func statusCode(_ status: AssetInventory.Status) -> Int32 {
    switch status {
    case .unsupported: return 0
    case .downloading: return 1
    case .supported: return 2
    case .installed: return 3
    @unknown default: return -1
    }
}

@_cdecl("mac_captions_speech_asset_status")
public func macCaptionsSpeechAssetStatus() -> Int32 {
    guard #available(macOS 27.0, *) else { return ShimError.unavailable.rawValue }
    do {
        return try runBlocking { statusCode(try await currentAssetStatus()) }
    } catch let error as ShimError {
        return error.rawValue
    } catch {
        return ShimError.analyzerFailed.rawValue
    }
}

private final class CallbackBox: @unchecked Sendable {
    let callback: MacCaptionsWordCallback
    let context: UnsafeMutableRawPointer?

    init(callback: MacCaptionsWordCallback, context: UnsafeMutableRawPointer?) {
        self.callback = callback
        self.context = context
    }

    func emit(_ text: String, _ start: Double, _ end: Double) {
        text.withCString { callback(context, $0, start, end) }
    }
}

@available(macOS 27.0, *)
private final class SpeechSession: @unchecked Sendable {
    private let transcriber: SpeechTranscriber
    private let analyzer: SpeechAnalyzer
    private let converter: AnalyzerInputConverter
    private let continuation: AsyncStream<AnalyzerInput>.Continuation
    private let callback: CallbackBox
    private let sourceFormat: AVAudioFormat
    private let sampleRate: Double
    private let reservedLocale: Locale
    private var ownsReservation: Bool
    private var nextSampleTime: AVAudioFramePosition = 0
    private var resultTask: Task<Void, Never>?

    init(sampleRate: UInt32, callback: CallbackBox) async throws {
        let (module, modules) = try await makeTranscriber()
        guard let locale = module.selectedLocales.first else { throw ShimError.localeUnavailable }
        let reservedBefore = await AssetInventory.reservedLocales
        let alreadyReserved = reservedBefore.contains(where: { $0.identifier == locale.identifier })
        let ownsReservation: Bool
        if alreadyReserved {
            ownsReservation = false
        } else {
            ownsReservation = try await AssetInventory.reserve(locale: locale)
            guard ownsReservation else { throw ShimError.reservationFailed }
        }

        guard await AssetInventory.status(forModules: modules) == .installed else {
            if ownsReservation { _ = await AssetInventory.release(reservedLocale: locale) }
            throw ShimError.assetsMissing
        }
        guard
            let format = AVAudioFormat(
                commonFormat: .pcmFormatFloat32,
                sampleRate: Double(sampleRate),
                channels: 1,
                interleaved: false
            ),
            let analyzerFormat = await SpeechAnalyzer.bestAvailableAudioFormat(
                compatibleWith: modules,
                considering: format
            )
        else {
            if ownsReservation { _ = await AssetInventory.release(reservedLocale: locale) }
            throw ShimError.formatUnavailable
        }

        let analyzer = SpeechAnalyzer(modules: modules)
        let converter = AnalyzerInputConverter(analyzerFormat: analyzerFormat)
        let (stream, streamContinuation) = AsyncStream<AnalyzerInput>.makeStream()
        do {
            try await analyzer.prepareToAnalyze(in: analyzerFormat)
            try await analyzer.start(inputSequence: stream)
        } catch {
            if ownsReservation { _ = await AssetInventory.release(reservedLocale: locale) }
            throw error
        }

        transcriber = module
        self.analyzer = analyzer
        self.converter = converter
        continuation = streamContinuation
        self.sourceFormat = format
        self.sampleRate = Double(sampleRate)
        self.callback = callback
        reservedLocale = locale
        self.ownsReservation = ownsReservation
        resultTask = Task {
            do {
                for try await result in transcriber.results where result.isFinal {
                    for run in result.text.runs {
                        guard let range = run.audioTimeRange,
                            range.start.isValid,
                            range.duration.isValid
                        else { continue }
                        let text = String(result.text[run.range].characters)
                        let start = CMTimeGetSeconds(range.start)
                        let duration = CMTimeGetSeconds(range.duration)
                        let words = text.split(whereSeparator: \.isWhitespace).map(String.init)
                        for (index, word) in words.enumerated() {
                            let wordStart = start + duration * Double(index) / Double(words.count)
                            let wordEnd = start + duration * Double(index + 1) / Double(words.count)
                            callback.emit(word, wordStart, wordEnd)
                        }
                    }
                }
            } catch {
                FileHandle.standardError.write("speech_results_error=\(error)\n".data(using: .utf8)!)
            }
        }
    }

    func push(_ samples: UnsafePointer<Float>, frames: UInt32) throws {
        guard let buffer = AVAudioPCMBuffer(pcmFormat: sourceFormat, frameCapacity: frames),
            let channel = buffer.floatChannelData?[0]
        else { throw ShimError.formatUnavailable }
        buffer.frameLength = frames
        channel.update(from: samples, count: Int(frames))
        let time = AVAudioTime(sampleTime: nextSampleTime, atRate: sampleRate)
        for input in try converter.convert(buffer, at: time) {
            continuation.yield(input)
        }
        nextSampleTime += AVAudioFramePosition(frames)
    }

    func stop() async {
        do {
            for input in try converter.flush() {
                continuation.yield(input)
            }
            continuation.finish()
            try await analyzer.finalizeAndFinishThroughEndOfInput()
        } catch {
            continuation.finish()
            await analyzer.cancelAndFinishNow()
            FileHandle.standardError.write("speech_finish_error=\(error)\n".data(using: .utf8)!)
        }
        await resultTask?.value
        resultTask = nil
        if ownsReservation {
            let released = await AssetInventory.release(reservedLocale: reservedLocale)
            ownsReservation = false
            if !released {
                FileHandle.standardError.write(
                    "speech_locale_release_failed=\(reservedLocale.identifier)\n".data(using: .utf8)!)
            }
        }
    }

    func feed(file: AVAudioFile) async throws {
        let chunkFrames: AVAudioFrameCount = 4096
        let totalFrames = file.length
        let epoch = ProcessInfo.processInfo.systemUptime
        var offset: AVAudioFramePosition = 0
        while offset < totalFrames {
            let count = min(AVAudioFramePosition(chunkFrames), totalFrames - offset)
            let deadline = epoch + Double(offset + count) / sampleRate
            let remaining = deadline - ProcessInfo.processInfo.systemUptime
            if remaining > 0 {
                try await Task.sleep(nanoseconds: UInt64(remaining * 1_000_000_000))
            }
            guard
                let buffer = AVAudioPCMBuffer(
                    pcmFormat: file.processingFormat,
                    frameCapacity: AVAudioFrameCount(count))
            else { throw ShimError.formatUnavailable }
            try file.read(into: buffer, frameCount: AVAudioFrameCount(count))
            guard let samples = buffer.floatChannelData?[0] else { throw ShimError.formatUnavailable }
            try push(samples, frames: buffer.frameLength)
            offset += AVAudioFramePosition(buffer.frameLength)
        }
        await stop()
    }
}

private final class SessionBox: @unchecked Sendable {
    var session: SpeechSession?
    var sampleRate: UInt32 = 0
    var callback: CallbackBox?
}

@_cdecl("mac_captions_speech_create")
public func macCaptionsSpeechCreate(_: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    Unmanaged.passRetained(SessionBox()).toOpaque()
}

@_cdecl("mac_captions_speech_start")
public func macCaptionsSpeechStart(
    _ opaque: UnsafeMutableRawPointer?,
    _ sampleRate: UInt32,
    _ callback: MacCaptionsWordCallback?,
    _ context: UnsafeMutableRawPointer?
) -> Bool {
    guard let opaque, let callback, sampleRate > 0,
        #available(macOS 27.0, *)
    else { return false }
    let box = Unmanaged<SessionBox>.fromOpaque(opaque).takeUnretainedValue()
    let callbackBox = CallbackBox(callback: callback, context: context)
    do {
        let session = try runBlocking { try await SpeechSession(sampleRate: sampleRate, callback: callbackBox) }
        box.sampleRate = sampleRate
        box.callback = callbackBox
        box.session = session
        return true
    } catch {
        FileHandle.standardError.write("speech_start_error=\(error)\n".data(using: .utf8)!)
        return false
    }
}

@_cdecl("mac_captions_speech_push")
public func macCaptionsSpeechPush(
    _ opaque: UnsafeMutableRawPointer?,
    _ samples: UnsafePointer<Float>?,
    _ frames: UInt32,
    _ timestamp: UInt64
) -> Bool {
    _ = timestamp
    guard let opaque, let samples, frames > 0,
        #available(macOS 27.0, *)
    else { return false }
    let box = Unmanaged<SessionBox>.fromOpaque(opaque).takeUnretainedValue()
    guard let session = box.session else { return false }
    do {
        try session.push(samples, frames: frames)
        return true
    } catch {
        FileHandle.standardError.write("speech_push_error=\(error)\n".data(using: .utf8)!)
        return false
    }
}

@_cdecl("mac_captions_speech_stop")
public func macCaptionsSpeechStop(_ opaque: UnsafeMutableRawPointer?) {
    guard let opaque, #available(macOS 27.0, *) else { return }
    let box = Unmanaged<SessionBox>.fromOpaque(opaque).takeUnretainedValue()
    guard let session = box.session else { return }
    do {
        try runBlocking { await session.stop() }
        box.session = nil
    } catch {
        FileHandle.standardError.write("speech_stop_error=\(error)\n".data(using: .utf8)!)
    }
}

@_cdecl("mac_captions_speech_destroy")
public func macCaptionsSpeechDestroy(_ opaque: UnsafeMutableRawPointer?) {
    guard let opaque else { return }
    let box = Unmanaged<SessionBox>.fromOpaque(opaque).takeRetainedValue()
    if #available(macOS 27.0, *), let session = box.session {
        try? runBlocking { await session.stop() }
        box.session = nil
    }
}

@_cdecl("mac_captions_speech_transcribe_file")
public func macCaptionsSpeechTranscribeFile(
    _ path: UnsafePointer<CChar>?,
    _ callback: MacCaptionsWordCallback?,
    _ context: UnsafeMutableRawPointer?
) -> Int32 {
    guard let path, let callback else { return ShimError.badArguments.rawValue }
    guard #available(macOS 27.0, *) else { return ShimError.unavailable.rawValue }
    let wavPath = String(cString: path)
    let callbackBox = CallbackBox(callback: callback, context: context)
    do {
        return try runBlocking {
            guard let file = try? AVAudioFile(forReading: URL(fileURLWithPath: wavPath)) else {
                return ShimError.badArguments.rawValue
            }
            let session = try await SpeechSession(
                sampleRate: UInt32(file.processingFormat.sampleRate),
                callback: callbackBox
            )
            do {
                try await session.feed(file: file)
                return ShimError.ok.rawValue
            } catch {
                await session.stop()
                throw error
            }
        }
    } catch let error as ShimError {
        return error.rawValue
    } catch {
        FileHandle.standardError.write("speech_file_error=\(error)\n".data(using: .utf8)!)
        return ShimError.analyzerFailed.rawValue
    }
}

#if MAC_CAPTIONS_ASSET_INSTALLER
    @main
    private struct SpeechAssetInstaller {
        static func main() async {
            guard CommandLine.arguments.count == 2,
                CommandLine.arguments[1] == "--install-en-us-speech-assets"
            else {
                FileHandle.standardError.write(
                    "Usage: install-speech-assets --install-en-us-speech-assets\n".data(using: .utf8)!
                )
                exit(64)
            }
            guard #available(macOS 27.0, *) else {
                FileHandle.standardError.write(
                    "Speech asset installation requires macOS 27 or later.\n".data(using: .utf8)!)
                exit(69)
            }
            do {
                try await installEnglishSpeechAssets()
            } catch {
                FileHandle.standardError.write(
                    "speech_asset_install_error=\(String(reflecting: error))\n".data(using: .utf8)!)
                exit(1)
            }
        }
    }
#endif

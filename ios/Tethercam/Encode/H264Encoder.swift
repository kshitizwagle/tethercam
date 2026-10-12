import CoreMedia
import Foundation
import VideoToolbox

/// Hardware H.264 encoder (VideoToolbox) configured for low latency (TECH_SPEC §5).
/// Output is Annex-B; parameter sets are reported separately for VideoConfig.
final class H264Encoder: @unchecked Sendable {
    struct Output {
        var annexB: Data
        var keyframe: Bool
        var ptsUs: UInt64
        var parameterSets: Data?  // set on keyframes
    }

    private var session: VTCompressionSession?
    private(set) var width: Int32 = 0
    private(set) var height: Int32 = 0
    private var fps: Int32 = 30
    private var bitrateKbps: Int = 12000
    private var forceKeyframe = true
    private let lock = NSLock()
    var onOutput: ((Output) -> Void)?
    var onError: ((OSStatus) -> Void)?

    deinit { invalidate() }

    func configure(fps: Int, bitrateKbps: Int) {
        lock.lock()
        defer { lock.unlock() }
        self.fps = Int32(fps)
        self.bitrateKbps = bitrateKbps
        if let s = session { applyRate(s) }
        forceKeyframe = true
    }

    func requestKeyframe() {
        lock.lock()
        forceKeyframe = true
        lock.unlock()
    }

    func invalidate() {
        lock.lock()
        defer { lock.unlock() }
        if let s = session {
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(s)
        }
        session = nil
        width = 0
        height = 0
    }

    func encode(_ sampleBuffer: CMSampleBuffer) {
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let w = Int32(CVPixelBufferGetWidth(pixelBuffer))
        let h = Int32(CVPixelBufferGetHeight(pixelBuffer))
        lock.lock()
        if session == nil || w != width || h != height {
            lock.unlock()
            invalidate()
            lock.lock()
            guard createSession(width: w, height: h) else {
                lock.unlock()
                onError?(-1)
                return
            }
        }
        let s = session!
        let key = forceKeyframe
        forceKeyframe = false
        lock.unlock()

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let props: CFDictionary? = key ? ([kVTEncodeFrameOptionKey_ForceKeyFrame: true] as [CFString: Any]) as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(
            s, imageBuffer: pixelBuffer, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: props, infoFlagsOut: nil
        ) { [weak self] status, _, buffer in
            guard let self else { return }
            guard status == noErr, let buffer else {
                self.onError?(status)
                return
            }
            self.handle(buffer)
        }
        if status != noErr { onError?(status) }
    }

    private func createSession(width w: Int32, height h: Int32) -> Bool {
        let spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true]
        var s: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: w, height: h, codecType: kCMVideoCodecType_H264,
            encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard status == noErr, let s else { return false }
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_ConstrainedHigh_AutoLevel)
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        applyRate(s)
        VTCompressionSessionPrepareToEncodeFrames(s)
        session = s
        width = w
        height = h
        forceKeyframe = true
        return true
    }

    private func applyRate(_ s: VTCompressionSession) {
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: NSNumber(value: fps))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bitrateKbps * 1000))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: NSNumber(value: fps * 2))
        VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: NSNumber(value: 2))
    }

    private func handle(_ buffer: CMSampleBuffer) {
        guard let block = CMSampleBufferGetDataBuffer(buffer) else { return }
        let length = CMBlockBufferGetDataLength(block)
        var avcc = Data(count: length)
        let copied = avcc.withUnsafeMutableBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: base)
        }
        guard copied == noErr else { return }

        var keyframe = true
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, createIfNecessary: false) as? [[CFString: Any]],
           let first = attachments.first,
           let notSync = first[kCMSampleAttachmentKey_NotSync] as? Bool {
            keyframe = !notSync
        }
        var parameterSets: Data?
        if keyframe, let fd = CMSampleBufferGetFormatDescription(buffer) {
            parameterSets = Self.parameterSets(fd)
        }
        let pts = CMSampleBufferGetPresentationTimeStamp(buffer)
        let ptsUs = pts.isValid ? UInt64(max(0, CMTimeGetSeconds(pts) * 1_000_000)) : 0
        onOutput?(Output(annexB: AnnexB.fromAVCC(avcc), keyframe: keyframe, ptsUs: ptsUs, parameterSets: parameterSets))
    }

    static func parameterSets(_ fd: CMFormatDescription) -> Data {
        var out = Data()
        var count = 0
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        for i in 0..<count {
            var ptr: UnsafePointer<UInt8>?
            var size = 0
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fd, parameterSetIndex: i, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let ptr {
                out.append(contentsOf: AnnexB.startCode)
                out.append(ptr, count: size)
            }
        }
        return out
    }
}

enum AnnexB {
    static let startCode: [UInt8] = [0, 0, 0, 1]

    /// Converts 4-byte big-endian length-prefixed NAL units (AVCC) to Annex-B.
    static func fromAVCC(_ avcc: Data) -> Data {
        var out = Data()
        out.reserveCapacity(avcc.count + 16)
        let bytes = [UInt8](avcc)
        var i = 0
        while i + 4 <= bytes.count {
            let len = Int(bytes[i]) << 24 | Int(bytes[i + 1]) << 16 | Int(bytes[i + 2]) << 8 | Int(bytes[i + 3])
            i += 4
            guard len <= bytes.count - i else { break }
            out.append(contentsOf: startCode)
            out.append(contentsOf: bytes[i..<(i + len)])
            i += len
        }
        return out
    }
}

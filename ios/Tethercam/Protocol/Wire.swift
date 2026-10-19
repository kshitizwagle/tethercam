import Foundation

// Tethercam wire protocol v1 (docs/PROTOCOL.md). Mirrors windows/common/src/protocol.cpp;
// both are tested against docs/protocol-vectors/v1.txt.

enum Wire {
    static let magic: [UInt8] = Array("TCAM".utf8)
    static let version: UInt8 = 1
    static let headerSize = 12
    static let maxPayload = 16 * 1024 * 1024
    static let devicePort: UInt16 = 5321
    /// Encrypted Wi-Fi transport (docs/PROTOCOL.md § Wireless transport).
    static let wifiPort: UInt16 = 5322
}

enum MessageType: UInt8 {
    case hello = 0x01
    case caps = 0x02
    case setCamera = 0x10
    case setFormat = 0x11
    case start = 0x12
    case stop = 0x13
    case requestKeyframe = 0x14
    case setOptions = 0x15
    case setQuality = 0x16
    case videoConfig = 0x20
    case videoFrame = 0x21
    case heartbeat = 0x30
    case status = 0x31
    case wifiPair = 0x40    // host -> device, over USB only
    case wifiPaired = 0x41  // device -> host, over USB only
    case error = 0x7F
}

enum Role: UInt8 { case host = 0, device = 1 }
enum CameraPosition: UInt8 { case back = 0, front = 1 }
enum Lens: UInt8 { case wide = 0, ultraWide = 1, telephoto = 2, front = 3 }

enum ErrorCode: UInt16 {
    case unsupportedVersion = 1
    case cameraPermissionDenied = 2
    case invalidFormat = 3
    case cameraUnavailable = 4
    case encoderFailure = 5
    case thermalShutdown = 6
}

struct VideoFormat: Equatable, Hashable, Sendable {
    var width: UInt16
    var height: UInt16
    var maxFps: UInt16
}

struct CameraInfo: Equatable, Sendable {
    var id: UInt8
    var position: CameraPosition
    var lens: Lens
    var label: String
    var formats: [VideoFormat]
}

enum CaptureQuality: UInt8, CaseIterable, Sendable {
    case auto = 0, hd30, hd60, uhd30, uhd60

    var label: String {
        switch self {
        case .auto: return "Auto"
        case .hd30: return "1080p / 30 fps"
        case .hd60: return "1080p / 60 fps"
        case .uhd30: return "4K / 30 fps"
        case .uhd60: return "4K / 60 fps"
        }
    }

    var format: VideoFormat? {
        switch self {
        case .auto: return nil
        case .hd30: return VideoFormat(width: 1920, height: 1080, maxFps: 30)
        case .hd60: return VideoFormat(width: 1920, height: 1080, maxFps: 60)
        case .uhd30: return VideoFormat(width: 3840, height: 2160, maxFps: 30)
        case .uhd60: return VideoFormat(width: 3840, height: 2160, maxFps: 60)
        }
    }

    func isSupported(by formats: [VideoFormat]) -> Bool {
        guard let wanted = format else { return true }
        return formats.contains { $0.width == wanted.width && $0.height == wanted.height && $0.maxFps >= wanted.maxFps }
    }
}

enum Message: Equatable, Sendable {
    case hello(minVersion: UInt8, maxVersion: UInt8, role: Role, appVersion: String, name: String, model: String)
    case caps(cameras: [CameraInfo], codecs: UInt8)
    case setCamera(id: UInt8)
    case setFormat(width: UInt16, height: UInt16, fps: UInt16, codec: UInt8, bitrateKbps: UInt32)
    case start
    case stop
    case requestKeyframe
    case setOptions(mirror: Bool, rotation: UInt16, dimPreview: Bool)
    case setQuality(CaptureQuality)
    case videoConfig(codec: UInt8, width: UInt16, height: UInt16, fps: UInt16, parameterSets: Data)
    case videoFrame(ptsUs: UInt64, keyframe: Bool, data: Data)
    case heartbeat(sendTimeUs: UInt64)
    case status(thermal: UInt8, battery: UInt8, charging: Bool, bitrateKbps: UInt32, fpsX100: UInt16, cameraId: UInt8)
    case error(code: UInt16, message: String)
    /// The host's key for later Wi-Fi sessions (16-byte host ID, 32-byte key).
    case wifiPair(hostId: Data, key: Data, name: String)
    /// status 0 = paired, 1 = Wireless is off.
    case wifiPaired(status: UInt8, deviceId: Data, name: String, port: UInt16, address: String)
    case unknown(type: UInt8)
}

// MARK: - Byte helpers

struct ByteWriter {
    private(set) var data = Data()

    mutating func u8(_ v: UInt8) { data.append(v) }
    mutating func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func u64(_ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { data.append(contentsOf: $0) } }
    mutating func bytes(_ d: Data) { data.append(d) }
    mutating func str(_ s: String) {
        var utf8 = Data(s.utf8)
        if utf8.count > 0xFFFF { utf8 = utf8.prefix(0xFFFF) }
        u16(UInt16(utf8.count))
        data.append(utf8)
    }
}

struct ByteReader {
    private let data: Data
    private var pos: Int
    private(set) var failed = false

    init(_ data: Data) {
        self.data = data
        self.pos = data.startIndex
    }

    var remaining: Int { data.endIndex - pos }

    private mutating func take(_ n: Int) -> Data? {
        guard !failed, n >= 0, remaining >= n else {
            failed = true
            return nil
        }
        let d = data[pos..<(pos + n)]
        pos += n
        return d
    }

    private mutating func int<T: FixedWidthInteger>(_: T.Type) -> T {
        guard let d = take(MemoryLayout<T>.size) else { return 0 }
        var v: T = 0
        for (i, b) in d.enumerated() { v |= T(b) << (8 * i) }
        return v
    }

    mutating func u8() -> UInt8 { int(UInt8.self) }
    mutating func u16() -> UInt16 { int(UInt16.self) }
    mutating func u32() -> UInt32 { int(UInt32.self) }
    mutating func u64() -> UInt64 { int(UInt64.self) }
    mutating func str() -> String {
        let n = Int(u16())
        guard let d = take(n) else { return "" }
        return String(decoding: d, as: UTF8.self)
    }
    mutating func bytes(_ n: Int) -> Data { take(n) ?? Data() }
    mutating func rest() -> Data { take(remaining) ?? Data() }
}

// MARK: - Encode / decode

extension Message {
    var typeByte: UInt8 {
        switch self {
        case .hello: return MessageType.hello.rawValue
        case .caps: return MessageType.caps.rawValue
        case .setCamera: return MessageType.setCamera.rawValue
        case .setFormat: return MessageType.setFormat.rawValue
        case .start: return MessageType.start.rawValue
        case .stop: return MessageType.stop.rawValue
        case .requestKeyframe: return MessageType.requestKeyframe.rawValue
        case .setOptions: return MessageType.setOptions.rawValue
        case .setQuality: return MessageType.setQuality.rawValue
        case .videoConfig: return MessageType.videoConfig.rawValue
        case .videoFrame: return MessageType.videoFrame.rawValue
        case .heartbeat: return MessageType.heartbeat.rawValue
        case .status: return MessageType.status.rawValue
        case .error: return MessageType.error.rawValue
        case .wifiPair: return MessageType.wifiPair.rawValue
        case .wifiPaired: return MessageType.wifiPaired.rawValue
        case .unknown(let t): return t
        }
    }

    /// Full frame: 12-byte header + payload.
    func encoded() -> Data {
        var p = ByteWriter()
        switch self {
        case let .hello(minV, maxV, role, appVersion, name, model):
            p.u8(minV); p.u8(maxV); p.u8(role.rawValue)
            p.str(appVersion); p.str(name); p.str(model)
        case let .caps(cameras, codecs):
            p.u8(UInt8(cameras.count))
            for c in cameras {
                p.u8(c.id); p.u8(c.position.rawValue); p.u8(c.lens.rawValue); p.str(c.label)
                p.u16(UInt16(c.formats.count))
                for f in c.formats { p.u16(f.width); p.u16(f.height); p.u16(f.maxFps) }
            }
            p.u8(codecs)
        case let .setCamera(id):
            p.u8(id)
        case let .setQuality(quality):
            p.u8(quality.rawValue)
        case let .setFormat(w, h, fps, codec, bitrate):
            p.u16(w); p.u16(h); p.u16(fps); p.u8(codec); p.u32(bitrate)
        case .start, .stop, .requestKeyframe, .unknown:
            break
        case let .setOptions(mirror, rotation, dim):
            p.u8(mirror ? 1 : 0); p.u16(rotation); p.u8(dim ? 1 : 0)
        case let .videoConfig(codec, w, h, fps, ps):
            p.u8(codec); p.u16(w); p.u16(h); p.u16(fps); p.u32(UInt32(ps.count)); p.bytes(ps)
        case let .videoFrame(pts, key, data):
            p.u64(pts); p.u8(key ? 1 : 0); p.bytes(data)
        case let .heartbeat(t):
            p.u64(t)
        case let .status(thermal, battery, charging, bitrate, fps, camera):
            p.u8(thermal); p.u8(battery); p.u8(charging ? 1 : 0); p.u32(bitrate); p.u16(fps); p.u8(camera)
        case let .error(code, message):
            p.u16(code); p.str(message)
        case let .wifiPair(hostId, key, name):
            p.bytes(Self.fixed(hostId, 16)); p.bytes(Self.fixed(key, 32)); p.str(name)
        case let .wifiPaired(status, deviceId, name, port, address):
            p.u8(status); p.bytes(Self.fixed(deviceId, 16)); p.str(name); p.u16(port); p.str(address)
        }
        var out = ByteWriter()
        out.bytes(Data(Wire.magic))
        out.u8(Wire.version)
        out.u8(typeByte)
        out.u16(0)
        out.u32(UInt32(p.data.count))
        out.bytes(p.data)
        return out.data
    }

    /// Exactly `n` bytes: truncated or zero-padded (IDs and keys are fixed-size on the wire).
    private static func fixed(_ d: Data, _ n: Int) -> Data {
        var out = Data(d.prefix(n))
        if out.count < n { out.append(Data(count: n - out.count)) }
        return out
    }

    /// Decodes a payload; nil for a malformed known message.
    static func decode(type: UInt8, payload: Data) -> Message? {
        var r = ByteReader(payload)
        let msg: Message
        switch MessageType(rawValue: type) {
        case .hello:
            let minV = r.u8(), maxV = r.u8()
            let role = Role(rawValue: r.u8()) ?? .host
            msg = .hello(minVersion: minV, maxVersion: maxV, role: role, appVersion: r.str(), name: r.str(), model: r.str())
        case .caps:
            var cameras: [CameraInfo] = []
            let n = Int(r.u8())
            for _ in 0..<n where !r.failed {
                let id = r.u8()
                let position = CameraPosition(rawValue: r.u8()) ?? .back
                let lens = Lens(rawValue: r.u8()) ?? .wide
                let label = r.str()
                let nf = Int(r.u16())
                if nf * 6 > r.remaining { return nil }
                var formats: [VideoFormat] = []
                for _ in 0..<nf { formats.append(VideoFormat(width: r.u16(), height: r.u16(), maxFps: r.u16())) }
                cameras.append(CameraInfo(id: id, position: position, lens: lens, label: label, formats: formats))
            }
            msg = .caps(cameras: cameras, codecs: r.u8())
        case .setCamera:
            msg = .setCamera(id: r.u8())
        case .setQuality:
            let raw = r.u8()
            if r.failed { return nil }
            // A preset this build doesn't know (from a newer peer) is ignored, not fatal.
            guard let quality = CaptureQuality(rawValue: raw) else { return .unknown(type: type) }
            msg = .setQuality(quality)
        case .setFormat:
            msg = .setFormat(width: r.u16(), height: r.u16(), fps: r.u16(), codec: r.u8(), bitrateKbps: r.u32())
        case .start: msg = .start
        case .stop: msg = .stop
        case .requestKeyframe: msg = .requestKeyframe
        case .setOptions:
            msg = .setOptions(mirror: r.u8() != 0, rotation: r.u16(), dimPreview: r.u8() != 0)
        case .videoConfig:
            let codec = r.u8(), w = r.u16(), h = r.u16(), fps = r.u16()
            let n = Int(r.u32())
            msg = .videoConfig(codec: codec, width: w, height: h, fps: fps, parameterSets: r.bytes(n))
        case .videoFrame:
            let pts = r.u64()
            let key = r.u8() & 1 != 0
            msg = .videoFrame(ptsUs: pts, keyframe: key, data: r.rest())
        case .heartbeat:
            msg = .heartbeat(sendTimeUs: r.u64())
        case .status:
            let thermal = r.u8(), battery = r.u8(), charging = r.u8() != 0, bitrate = r.u32(), fps = r.u16()
            let camera: UInt8 = r.remaining >= 1 ? r.u8() : 255  // optional trailing field
            msg = .status(thermal: thermal, battery: battery, charging: charging, bitrateKbps: bitrate, fpsX100: fps, cameraId: camera)
        case .error:
            msg = .error(code: r.u16(), message: r.str())
        case .wifiPair:
            msg = .wifiPair(hostId: Data(r.bytes(16)), key: Data(r.bytes(32)), name: r.str())
        case .wifiPaired:
            let status = r.u8()
            let deviceId = Data(r.bytes(16))
            msg = .wifiPaired(status: status, deviceId: deviceId, name: r.str(), port: r.u16(), address: r.str())
        case .none:
            return .unknown(type: type)
        }
        return r.failed ? nil : msg
    }
}

/// Incremental parser for the TCP byte stream.
struct StreamParser {
    enum Result { case needMore, message(Message), fatal }

    private var buffer = Data()

    mutating func feed(_ data: Data) { buffer.append(data) }

    mutating func next() -> Result {
        guard buffer.count >= Wire.headerSize else { return .needMore }
        let base = buffer.startIndex
        guard Array(buffer[base..<(base + 4)]) == Wire.magic else { return .fatal }
        let type = buffer[base + 5]
        let len = Int(buffer[base + 8]) | Int(buffer[base + 9]) << 8 | Int(buffer[base + 10]) << 16 | Int(buffer[base + 11]) << 24
        guard len <= Wire.maxPayload else { return .fatal }
        guard buffer.count >= Wire.headerSize + len else { return .needMore }
        let payload = buffer.subdata(in: (base + Wire.headerSize)..<(base + Wire.headerSize + len))
        buffer = buffer.subdata(in: (base + Wire.headerSize + len)..<buffer.endIndex)
        guard let msg = Message.decode(type: type, payload: payload) else { return .fatal }
        return .message(msg)
    }
}

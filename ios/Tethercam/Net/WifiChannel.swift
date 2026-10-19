import Foundation

/// Device side of the Wi-Fi transport (docs/PROTOCOL.md § Wireless transport): the 24-byte preamble,
/// the Noise responder on the host's pairing key, then length-prefixed encrypted records carrying the
/// ordinary TCAM byte stream. Mirrors wifi::AcceptDevice and SecureLink in common/src/link.cpp.
/// Platform-neutral; not thread-safe (Server uses it on its queue).
final class WifiChannel {
    enum Event: Equatable {
        case needMore
        /// Handshake done: send `reply` to the host as-is.
        case established(reply: Data)
        case failed
    }

    static let preambleSize = 24
    static let magic: [UInt8] = Array("TCWF".utf8)

    private let lookup: (Data) -> Data?
    private var inbox: [UInt8] = []
    private var handshake: Noise.Handshake?
    private var sendCipher = Noise.CipherState()
    private var recvCipher = Noise.CipherState()
    private(set) var hostId: Data?
    private(set) var established = false

    /// `lookup` returns the 32-byte key for a paired host ID, or nil.
    init(lookup: @escaping (Data) -> Data?) { self.lookup = lookup }

    static func preamble(hostId: Data) -> [UInt8] {
        var p = magic + [1, 0, 0, 0]
        p += Array(hostId.prefix(16))
        return p
    }

    /// Feeds bytes received before the handshake is done.
    func feedHandshake(_ data: Data) -> Event {
        inbox += data
        if handshake == nil {
            guard inbox.count >= Self.preambleSize else { return .needMore }
            let pre = Array(inbox[0..<Self.preambleSize])
            guard Array(pre[0..<4]) == Self.magic, pre[4] == 1 else { return .failed }
            let host = Data(pre[8..<24])
            guard let key = lookup(host), key.count == Noise.keySize else { return .failed }
            hostId = host
            handshake = Noise.Handshake(initiator: false, prologue: pre, psk: [UInt8](key))
            inbox.removeFirst(Self.preambleSize)
        }
        guard inbox.count >= 2 else { return .needMore }
        let len = Int(inbox[0]) | Int(inbox[1]) << 8
        guard inbox.count >= 2 + len else { return .needMore }
        let message = Array(inbox[2..<(2 + len)])
        inbox.removeFirst(2 + len)
        guard let hs = handshake, hs.read(message) != nil, let reply = hs.write(), hs.done else { return .failed }
        (sendCipher, recvCipher) = hs.split()
        handshake = nil
        established = true
        return .established(reply: Data([UInt8(reply.count & 0xFF), UInt8(reply.count >> 8)] + reply))
    }

    /// Encrypts `plaintext` into one or more records.
    func seal(_ plaintext: Data) -> Data {
        var out = Data()
        let bytes = [UInt8](plaintext)
        var offset = 0
        repeat {
            let n = min(Noise.maxPlaintext, bytes.count - offset)
            let ct = sendCipher.encrypt(ad: [], Array(bytes[offset..<(offset + n)]))
            let len = UInt32(ct.count)
            out.append(contentsOf: [UInt8(len & 0xFF), UInt8((len >> 8) & 0xFF), UInt8((len >> 16) & 0xFF), UInt8(len >> 24)])
            out.append(contentsOf: ct)
            offset += n
        } while offset < bytes.count
        return out
    }

    /// Decrypts every complete record; nil if one fails authentication or is malformed (close the
    /// connection). Call with empty data after `established` to process bytes that arrived with the
    /// handshake.
    func open(_ data: Data) -> Data? {
        inbox += data
        var out = Data()
        var consumed = 0
        while inbox.count - consumed >= 4 {
            let b = consumed
            let len = Int(inbox[b]) | Int(inbox[b + 1]) << 8 | Int(inbox[b + 2]) << 16 | Int(inbox[b + 3]) << 24
            guard len >= Noise.tagSize, len <= Noise.maxMessage else { return nil }
            guard inbox.count - consumed >= 4 + len else { break }
            guard let p = recvCipher.decrypt(ad: [], Array(inbox[(b + 4)..<(b + 4 + len)])) else { return nil }
            out.append(contentsOf: p)
            consumed += 4 + len
        }
        inbox.removeFirst(consumed)
        return out
    }
}

import Foundation

/// Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b on Monocypher (third_party/monocypher, through the bridging
/// header). Mirrors common/src/noise.cpp; both pass the same public test vector. Platform-neutral.
enum Noise {
    static let keySize = 32
    static let tagSize = 16
    static let hashSize = 64
    static let maxMessage = 65535  // Noise's limit on one message, tag included
    static let maxPlaintext = maxMessage - tagSize
    static let protocolName = "Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b"

    static func random(_ count: Int) -> [UInt8] {
        var rng = SystemRandomNumberGenerator()  // the OS CSPRNG
        return (0..<count).map { _ in UInt8.random(in: 0...255, using: &rng) }
    }

    static func blake2b(_ parts: [[UInt8]]) -> [UInt8] {
        var ctx = crypto_blake2b_ctx()
        crypto_blake2b_init(&ctx, hashSize)
        for p in parts { p.withUnsafeBufferPointer { crypto_blake2b_update(&ctx, $0.baseAddress, $0.count) } }
        var out = [UInt8](repeating: 0, count: hashSize)
        crypto_blake2b_final(&ctx, &out)
        return out
    }

    /// HMAC-BLAKE2b (RFC 2104); keys here are never longer than a block.
    static func hmac(_ key: [UInt8], _ data: [[UInt8]]) -> [UInt8] {
        let pad = key + [UInt8](repeating: 0, count: 128 - key.count)
        let inner = blake2b([pad.map { $0 ^ 0x36 }] + data)
        return blake2b([pad.map { $0 ^ 0x5c }, inner])
    }

    static func hkdf(_ ck: [UInt8], _ ikm: [UInt8], outputs: Int) -> [[UInt8]] {
        let temp = hmac(ck, [ikm])
        var out: [[UInt8]] = []
        var prev: [UInt8] = []
        for i in 1...outputs {
            prev = hmac(temp, [prev, [UInt8(i)]])
            out.append(prev)
        }
        return out
    }

    /// ChaCha20-Poly1305 with Noise's nonce layout (32 zero bits, then a 64-bit little-endian counter).
    struct CipherState {
        private var key = [UInt8](repeating: 0, count: Noise.keySize)
        private var n: UInt64 = 0

        init() {}
        init(key: [UInt8]) { setKey(key) }

        mutating func setKey(_ k: [UInt8]) {
            key = Array(k.prefix(Noise.keySize))
            n = 0
        }

        private static func nonce(_ n: UInt64) -> [UInt8] {
            [0, 0, 0, 0] + (0..<8).map { UInt8(truncatingIfNeeded: n >> (8 * UInt64($0))) }
        }

        /// Ciphertext followed by the 16-byte tag.
        mutating func encrypt(ad: [UInt8], _ plaintext: [UInt8]) -> [UInt8] {
            var ctx = crypto_aead_ctx()
            crypto_aead_init_ietf(&ctx, key, Self.nonce(n))
            n += 1
            var out = [UInt8](repeating: 0, count: plaintext.count + Noise.tagSize)
            let len = plaintext.count
            out.withUnsafeMutableBufferPointer { o in
                plaintext.withUnsafeBufferPointer { p in
                    crypto_aead_write(&ctx, o.baseAddress, o.baseAddress! + len, ad, ad.count, p.baseAddress, len)
                }
            }
            return out
        }

        /// nil if the tag doesn't verify (the nonce is not advanced; drop the connection).
        mutating func decrypt(ad: [UInt8], _ ciphertext: [UInt8]) -> [UInt8]? {
            guard ciphertext.count >= Noise.tagSize else { return nil }
            let len = ciphertext.count - Noise.tagSize
            var ctx = crypto_aead_ctx()
            crypto_aead_init_ietf(&ctx, key, Self.nonce(n))
            var out = [UInt8](repeating: 0, count: len)
            let bad = ciphertext.withUnsafeBufferPointer { c in
                out.withUnsafeMutableBufferPointer { o in
                    crypto_aead_read(&ctx, o.baseAddress, c.baseAddress! + len, ad, ad.count, c.baseAddress, len)
                }
            }
            guard bad == 0 else { return nil }
            n += 1
            return out
        }
    }

    /// One NNpsk0 handshake: -> psk, e   <- e, ee
    final class Handshake {
        let initiator: Bool
        private(set) var step = 0
        private var ck: [UInt8]
        private(set) var hash: [UInt8]
        private var cs = CipherState()
        private let psk: [UInt8]
        private var ePriv: [UInt8]
        private var ePub = [UInt8](repeating: 0, count: 32)
        private var re = [UInt8](repeating: 0, count: 32)
        private let fixedEphemeral: Bool

        /// `ephemeral` fixes the ephemeral private key (test vectors only).
        init(initiator: Bool, prologue: [UInt8], psk: [UInt8], ephemeral: [UInt8]? = nil) {
            self.initiator = initiator
            var name = Array(Noise.protocolName.utf8)
            name += [UInt8](repeating: 0, count: Noise.hashSize - name.count)
            ck = name
            hash = name
            self.psk = psk
            ePriv = ephemeral ?? [UInt8](repeating: 0, count: 32)
            fixedEphemeral = ephemeral != nil
            mixHash(prologue)
        }

        var done: Bool { step == 2 }

        private func mixHash(_ data: [UInt8]) { hash = Noise.blake2b([hash, data]) }

        private func mixKey(_ ikm: [UInt8]) {
            let o = Noise.hkdf(ck, ikm, outputs: 2)
            ck = o[0]
            cs.setKey(o[1])
        }

        private func mixKeyAndHash(_ ikm: [UInt8]) {
            let o = Noise.hkdf(ck, ikm, outputs: 3)
            ck = o[0]
            mixHash(o[1])
            cs.setKey(o[2])
        }

        private func writeEphemeral() -> [UInt8] {
            if !fixedEphemeral { ePriv = Noise.random(32) }
            var pub = [UInt8](repeating: 0, count: 32)
            crypto_x25519_public_key(&pub, ePriv)
            ePub = pub
            mixHash(ePub)
            mixKey(ePub)  // psk handshakes mix the ephemeral into the key as well
            return ePub
        }

        private func dh() -> [UInt8] {
            var shared = [UInt8](repeating: 0, count: 32)
            crypto_x25519(&shared, ePriv, re)
            return shared
        }

        /// Next handshake message carrying `payload`; nil if it isn't this side's turn.
        func write(_ payload: [UInt8] = []) -> [UInt8]? {
            guard initiator ? step == 0 : step == 1 else { return nil }
            var out: [UInt8]
            if initiator {
                mixKeyAndHash(psk)
                out = writeEphemeral()
            } else {
                out = writeEphemeral()
                mixKey(dh())
            }
            let c = cs.encrypt(ad: hash, payload)
            mixHash(c)
            out += c
            step += 1
            return out
        }

        /// The peer's payload, or nil if the message is malformed or fails authentication.
        func read(_ message: [UInt8]) -> [UInt8]? {
            guard initiator ? step == 1 : step == 0, message.count >= 32 + Noise.tagSize,
                  message.count <= Noise.maxMessage else { return nil }
            if !initiator { mixKeyAndHash(psk) }
            re = Array(message[0..<32])
            mixHash(re)
            mixKey(re)
            if initiator { mixKey(dh()) }
            let c = Array(message[32...])
            guard let p = cs.decrypt(ad: hash, c) else { return nil }
            mixHash(c)
            step += 1
            return p
        }

        /// After `done`: the transport ciphers for this side.
        func split() -> (send: CipherState, recv: CipherState) {
            let o = Noise.hkdf(ck, [], outputs: 2)
            let a = CipherState(key: o[0]), b = CipherState(key: o[1])
            return initiator ? (a, b) : (b, a)
        }
    }
}

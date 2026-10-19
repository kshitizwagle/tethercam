import XCTest

@testable import Tethercam

/// The Wi-Fi transport crypto, against the same public vector as common/tests/wifi_test.cpp.
final class NoiseTests: XCTestCase {
    private func hex(_ s: String) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            let j = s.index(i, offsetBy: 2)
            out.append(UInt8(s[i..<j], radix: 16)!)
            i = j
        }
        return out
    }

    // Noise_NNpsk0_25519_ChaChaPoly_BLAKE2b from the cacophony test suite (public domain).
    func testCacophonyVector() {
        let prologue = hex("4a6f686e2047616c74")
        let psk = hex("54686973206973206d7920417573747269616e20706572737065637469766521")
        let i = Noise.Handshake(initiator: true, prologue: prologue, psk: psk,
                                ephemeral: hex("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a"))
        let r = Noise.Handshake(initiator: false, prologue: prologue, psk: psk,
                                ephemeral: hex("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b"))
        let m1 = i.write(hex("4c756477696720766f6e204d69736573"))!
        XCTAssertEqual(m1, hex("ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944ed63df2f5a12aee1185ee9c50305f2ecf12421dcb53c047a63b784cf7c54a105"))
        XCTAssertNotNil(r.read(m1))
        let m2 = r.write(hex("4d757272617920526f746862617264"))!
        XCTAssertEqual(m2, hex("95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088430e0fe7d5c0c9b92f7478716c8852f1b4f389edb75e3ebe546fafafd5b6f0f7"))
        XCTAssertNotNil(i.read(m2))
        var (send, _) = i.split()
        XCTAssertEqual(send.encrypt(ad: [], hex("462e20412e20486179656b")), hex("0fe04b26dcfbb69f8a6a94c61f1a26ef88769218d32f5d17c068a6"))
    }

    func testChannelRefusesUnknownHostAndTamperedRecords() {
        let hostId = Data(repeating: 7, count: 16)
        let key = Data(Noise.random(32))
        let channel = WifiChannel { $0 == hostId ? key : nil }
        let pre = WifiChannel.preamble(hostId: hostId)
        let host = Noise.Handshake(initiator: true, prologue: pre, psk: [UInt8](key))
        let m1 = host.write()!
        guard case let .established(reply) = channel.feedHandshake(Data(pre + [UInt8(m1.count), 0] + m1)) else {
            return XCTFail("handshake")
        }
        XCTAssertNotNil(host.read(Array([UInt8](reply)[2...])))
        var (hostSend, _) = host.split()
        let ct = hostSend.encrypt(ad: [], Array("hi".utf8))
        var record = [UInt8(ct.count), 0, 0, 0] + ct
        record[5] ^= 1
        XCTAssertNil(channel.open(Data(record)))

        XCTAssertEqual(WifiChannel { _ in nil }.feedHandshake(Data(pre)), .failed)
    }
}

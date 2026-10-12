import XCTest

@testable import Tethercam

/// Decodes and re-encodes every golden vector shared with the Windows tests.
final class WireTests: XCTestCase {
    private func vectors() throws -> [String: Data] {
        let url = try XCTUnwrap(Bundle(for: WireTests.self).url(forResource: "v1", withExtension: "txt"))
        let text = try String(contentsOf: url, encoding: .utf8)
        var out: [String: Data] = [:]
        for line in text.split(separator: "\n") where !line.hasPrefix("#") && !line.trimmingCharacters(in: .whitespaces).isEmpty {
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2 else { continue }
            let hex = parts[1].filter(\.isHexDigit)
            var data = Data()
            var i = hex.startIndex
            while i < hex.endIndex {
                let j = hex.index(i, offsetBy: 2)
                data.append(UInt8(hex[i..<j], radix: 16)!)
                i = j
            }
            out[String(parts[0])] = data
        }
        return out
    }

    private func decodeOne(_ frame: Data) throws -> Message {
        var parser = StreamParser()
        parser.feed(frame)
        guard case .message(let m) = parser.next() else {
            XCTFail("vector did not decode")
            throw NSError(domain: "WireTests", code: 1)
        }
        return m
    }

    func testAllVectorsRoundTrip() throws {
        let v = try vectors()
        XCTAssertEqual(v.count, 13)
        for (name, bytes) in v {
            let m = try decodeOne(bytes)
            XCTAssertEqual(m.encoded(), bytes, name)
        }
    }

    func testDecodedValues() throws {
        let v = try vectors()
        guard case let .setFormat(w, h, fps, _, _) = try decodeOne(v["set_format_1080p30"]!) else { return XCTFail() }
        XCTAssertEqual([w, h, fps], [1920, 1080, 30])
        guard case let .caps(cameras, _) = try decodeOne(v["caps_one_camera"]!) else { return XCTFail() }
        XCTAssertEqual(cameras.first?.formats.first, VideoFormat(width: 1920, height: 1080, maxFps: 60))
        guard case let .videoFrame(pts, key, data) = try decodeOne(v["video_frame_idr"]!) else { return XCTFail() }
        XCTAssertEqual(pts, 1_000_000)
        XCTAssertTrue(key)
        XCTAssertEqual([UInt8](data), [0, 0, 0, 1, 0x65, 0x88])
    }

    func testParserRejectsBadMagic() {
        var parser = StreamParser()
        parser.feed(Data([0x58, 0x43, 0x41, 0x4D, 1, 1, 0, 0, 0, 0, 0, 0]))
        guard case .fatal = parser.next() else { return XCTFail() }
    }

    func testAvccToAnnexB() {
        let avcc = Data([0, 0, 0, 2, 0x41, 7, 0, 0, 0, 1, 0x06])
        XCTAssertEqual([UInt8](AnnexB.fromAVCC(avcc)), [0, 0, 0, 1, 0x41, 7, 0, 0, 0, 1, 0x06])
    }

    func testBitrateTableMatchesWindows() {
        XCTAssertEqual(Double(Session.defaultBitrate(1920, 1080, 30)), 12000, accuracy: 100)
        XCTAssertEqual(Double(Session.defaultBitrate(3840, 2160, 30)), 40000, accuracy: 2500)
    }
}

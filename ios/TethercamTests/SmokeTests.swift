import XCTest

@testable import Tethercam

final class SmokeTests: XCTestCase {
    func testAppModuleLoads() {
        XCTAssertNotNil(ContentView.self)
    }
}

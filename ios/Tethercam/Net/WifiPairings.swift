import Foundation
import Security

/// PCs paired for Wi-Fi (docs/PROTOCOL.md § WifiPair): host ID → 32-byte key and name, received over
/// USB only. Kept in the Keychain (this device only, readable after first unlock) together with this
/// phone's random device ID. Thread-safe.
final class WifiPairings: @unchecked Sendable {
    struct Host: Codable, Equatable, Identifiable, Sendable {
        var hostId: Data
        var key: Data
        var name: String
        var id: Data { hostId }
    }

    private static let service = "org.tethercam.wifi"
    private let lock = NSLock()
    private var hosts: [Host]
    let deviceId: Data

    init() {
        if let id = Self.read("deviceId"), id.count == 16 {
            deviceId = id
        } else {
            deviceId = Data(Noise.random(16))
            Self.write("deviceId", deviceId)
        }
        hosts = Self.read("hosts").flatMap { try? JSONDecoder().decode([Host].self, from: $0) } ?? []
    }

    var deviceIdHex: String { deviceId.map { String(format: "%02x", $0) }.joined() }

    var all: [Host] {
        lock.lock(); defer { lock.unlock() }
        return hosts
    }

    func key(for hostId: Data) -> Data? {
        lock.lock(); defer { lock.unlock() }
        return hosts.first { $0.hostId == hostId }?.key
    }

    func store(hostId: Data, key: Data, name: String) {
        lock.lock()
        hosts.removeAll { $0.hostId == hostId }
        hosts.insert(Host(hostId: hostId, key: key, name: name), at: 0)
        let snapshot = hosts
        lock.unlock()
        save(snapshot)
    }

    func forget(hostId: Data) {
        lock.lock()
        hosts.removeAll { $0.hostId == hostId }
        let snapshot = hosts
        lock.unlock()
        save(snapshot)
    }

    private func save(_ list: [Host]) {
        if let data = try? JSONEncoder().encode(list) { Self.write("hosts", data) }
    }

    // MARK: Keychain

    private static func query(_ account: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    private static func read(_ account: String) -> Data? {
        var q = query(account)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }

    private static func write(_ account: String, _ data: Data) {
        let attrs: [String: Any] = [kSecValueData as String: data,
                                    kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        if SecItemUpdate(query(account) as CFDictionary, attrs as CFDictionary) == errSecItemNotFound {
            SecItemAdd(query(account).merging(attrs) { $1 } as CFDictionary, nil)
        }
    }

    // MARK: Network

    /// The phone's current Wi-Fi IPv4 address (interface en0), reported to the PC in WifiPaired.
    static func wifiAddress() -> String {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return "" }
        defer { freeifaddrs(list) }
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = p.pointee
            guard String(cString: ifa.ifa_name) == "en0", let addr = ifa.ifa_addr,
                  addr.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                return String(cString: host)
            }
        }
        return ""
    }
}

import Foundation
import Network

/// TCP servers for the host connection.
/// - USB: 127.0.0.1:5321. usbmuxd delivers the PC's USB tunnel to the phone's loopback, so binding to
///   loopback keeps this port unreachable over Wi-Fi or cellular (TECH_SPEC N5).
/// - Wi-Fi (M11, opt-in): port 5322 on Wi-Fi/Ethernet only, advertised over Bonjour. Every connection
///   must complete the Noise handshake on a key a PC handed over USB (WifiChannel) before the session
///   sees it; everything after that is encrypted.
/// One client at a time: a new connection on either port replaces the old one.
final class Server: @unchecked Sendable {
    private let queue: DispatchQueue
    private var listener: NWListener?
    private var wifiListener: NWListener?
    private var connection: NWConnection?
    private var channel: WifiChannel?  // set while the current connection is a Wi-Fi one
    private var pendingBytes = 0
    private var listenerReady = false
    private var wifiEnabled = false

    var onConnect: (() -> Void)?
    var onDisconnect: (() -> Void)?
    var onData: ((Data) -> Void)?
    var onListenerState: ((String) -> Void)?
    /// Wi-Fi listener state for the UI ("" when ready or off).
    var onWifiState: ((String) -> Void)?
    /// Key for a paired host ID (Wi-Fi handshake).
    var keyLookup: ((Data) -> Data?)?
    /// Advertised in the Bonjour TXT record so a paired PC recognises this phone.
    var deviceIdHex = ""

    init(queue: DispatchQueue) { self.queue = queue }

    /// Bytes handed to the network stack but not yet sent; used for backpressure.
    var bytesInFlight: Int { pendingBytes }
    /// A session is open (for Wi-Fi: only once the handshake is done).
    var isConnected: Bool { connection != nil && (channel == nil || channel!.established) }
    var isWifi: Bool { channel != nil }
    /// The paired PC on the current Wi-Fi connection.
    var wifiHostId: Data? { channel?.hostId }

    func start() {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: Wire.devicePort)!)
        if let tcp = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options { tcp.noDelay = true }
        do {
            let l = try NWListener(using: params)
            l.stateUpdateHandler = { [weak self, weak l] state in
                guard let self, let l, self.listener === l else { return }
                switch state {
                case .ready:
                    self.listenerReady = true
                    self.onListenerState?("Listening")
                case .failed(let e):
                    self.listenerReady = false
                    self.onListenerState?("Listener failed: \(e)")
                    self.restartLater()
                case .waiting, .cancelled:
                    // iOS tears the socket down while the app is in the background; listen again.
                    self.listenerReady = false
                    self.restartLater()
                default: break
                }
            }
            l.newConnectionHandler = { [weak self] c in self?.accept(c, wifi: false) }
            l.start(queue: queue)
            listener = l
        } catch {
            onListenerState?("Cannot listen: \(error)")
            restartLater()
        }
    }

    private func restartLater(after seconds: Double = 1) {
        let old = listener
        listener = nil
        listenerReady = false
        old?.cancel()
        queue.asyncAfter(deadline: .now() + seconds) { [weak self] in
            guard let self, self.listener == nil else { return }
            self.start()
        }
    }

    /// Called when the app returns to the foreground: iOS may have closed the listening sockets while
    /// the app was suspended, so always start fresh listeners (the old ones may report .ready but be
    /// dead). Must be called on `queue`.
    func relisten() {
        guard connection == nil else { return }  // a live session means the socket is fine
        restartLater(after: 0.2)
        if wifiEnabled { restartWifiLater(after: 0.2) }
    }

    // MARK: Wi-Fi

    /// Turns the Wi-Fi listener and Bonjour advert on or off. Must be called on `queue`.
    func setWifi(enabled: Bool) {
        guard enabled != wifiEnabled else { return }
        wifiEnabled = enabled
        if enabled {
            startWifi()
        } else {
            wifiListener?.cancel()
            wifiListener = nil
            onWifiState?("")
            if isWifi { connection?.cancel() }
        }
    }

    private func startWifi() {
        guard wifiEnabled, wifiListener == nil else { return }
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.prohibitedInterfaceTypes = [.cellular]  // local network only
        params.includePeerToPeer = false
        if let tcp = params.defaultProtocolStack.transportProtocol as? NWProtocolTCP.Options { tcp.noDelay = true }
        do {
            let l = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Wire.wifiPort)!)
            l.service = NWListener.Service(type: "_tethercam._tcp", txtRecord: NWTXTRecord(["v": "1", "id": deviceIdHex]))
            l.stateUpdateHandler = { [weak self, weak l] state in
                guard let self, let l, self.wifiListener === l else { return }
                switch state {
                case .ready:
                    self.onWifiState?("")
                case .waiting(let e):
                    // Typically the Local Network permission is off, or there's no Wi-Fi.
                    self.onWifiState?("Wi-Fi unavailable: allow Local Network for Tethercam in Settings (\(e))")
                case .failed(let e):
                    self.onWifiState?("Wi-Fi listener failed: \(e)")
                    self.restartWifiLater(after: 2)
                default: break
                }
            }
            l.newConnectionHandler = { [weak self] c in self?.accept(c, wifi: true) }
            l.start(queue: queue)
            wifiListener = l
        } catch {
            onWifiState?("Cannot listen on Wi-Fi: \(error)")
            restartWifiLater(after: 2)
        }
    }

    private func restartWifiLater(after seconds: Double) {
        wifiListener?.cancel()
        wifiListener = nil
        queue.asyncAfter(deadline: .now() + seconds) { [weak self] in self?.startWifi() }
    }

    // MARK: Connections

    private func accept(_ c: NWConnection, wifi: Bool) {
        if let old = connection {
            let wasOpen = isConnected
            old.cancel()
            connection = nil
            channel = nil
            if wasOpen { onDisconnect?() }
        }
        connection = c
        pendingBytes = 0
        channel = wifi ? WifiChannel(lookup: { [weak self] id in self?.keyLookup?(id) }) : nil
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c else { return }
            switch state {
            case .ready:
                if !wifi { self.onConnect?() }  // Wi-Fi: after the handshake
                self.receive(on: c)
            case .failed, .cancelled:
                if self.connection === c {
                    let wasOpen = self.isConnected
                    self.connection = nil
                    self.channel = nil
                    if wasOpen { self.onDisconnect?() }
                }
            default: break
            }
        }
        c.start(queue: queue)
        if wifi {
            // A peer that never finishes the handshake doesn't keep the slot.
            queue.asyncAfter(deadline: .now() + 5) { [weak self, weak c] in
                guard let self, let c, self.connection === c, let ch = self.channel, !ch.established else { return }
                c.cancel()
            }
        }
    }

    private func receive(on c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, complete, error in
            guard let self, self.connection === c else { return }
            if let data, !data.isEmpty, !self.deliver(data, on: c) {
                c.cancel()
                return
            }
            if complete || error != nil {
                c.cancel()
                return
            }
            self.receive(on: c)
        }
    }

    /// False if the connection must be dropped (failed handshake or a bad record).
    private func deliver(_ data: Data, on c: NWConnection) -> Bool {
        guard let ch = channel else {
            onData?(data)
            return true
        }
        if !ch.established {
            switch ch.feedHandshake(data) {
            case .needMore:
                return true
            case .failed:
                return false
            case .established(let reply):
                c.send(content: reply, completion: .contentProcessed { _ in })
                onConnect?()
                guard let rest = ch.open(Data()) else { return false }  // bytes that came with the handshake
                if !rest.isEmpty { onData?(rest) }
                return true
            }
        }
        guard let plain = ch.open(data) else { return false }
        if !plain.isEmpty { onData?(plain) }
        return true
    }

    /// Must be called on `queue`.
    func send(_ data: Data) {
        guard let c = connection, isConnected else { return }
        let out = channel.map { $0.seal(data) } ?? data
        pendingBytes += out.count
        c.send(content: out, completion: .contentProcessed { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.pendingBytes = max(0, self.pendingBytes - out.count) }
        })
    }

    func disconnect() {
        connection?.cancel()
    }
}

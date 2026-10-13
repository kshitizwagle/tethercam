import Foundation
import Network

/// TCP server on 127.0.0.1:5321. usbmuxd delivers the PC's USB tunnel to the phone's loopback,
/// so binding to loopback keeps the port unreachable over Wi-Fi or cellular (TECH_SPEC N5).
/// One client at a time: a new connection replaces the old one.
final class Server: @unchecked Sendable {
    private let queue: DispatchQueue
    private var listener: NWListener?
    private var connection: NWConnection?
    private var pendingBytes = 0
    private var listenerReady = false

    var onConnect: (() -> Void)?
    var onDisconnect: (() -> Void)?
    var onData: ((Data) -> Void)?
    var onListenerState: ((String) -> Void)?

    init(queue: DispatchQueue) { self.queue = queue }

    /// Bytes handed to the network stack but not yet sent; used for backpressure.
    var bytesInFlight: Int { pendingBytes }
    var isConnected: Bool { connection != nil }

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
            l.newConnectionHandler = { [weak self] c in self?.accept(c) }
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

    /// Called when the app returns to the foreground: iOS may have closed the listening socket while
    /// the app was suspended, so always start a fresh listener (the old one may report .ready but be dead).
    /// Must be called on `queue`.
    func relisten() {
        guard connection == nil else { return }  // a live session means the socket is fine
        restartLater(after: 0.2)
    }

    private func accept(_ c: NWConnection) {
        if let old = connection {
            old.cancel()
            connection = nil
            onDisconnect?()
        }
        connection = c
        pendingBytes = 0
        c.stateUpdateHandler = { [weak self, weak c] state in
            guard let self, let c else { return }
            switch state {
            case .ready:
                self.onConnect?()
                self.receive(on: c)
            case .failed, .cancelled:
                if self.connection === c {
                    self.connection = nil
                    self.onDisconnect?()
                }
            default: break
            }
        }
        c.start(queue: queue)
    }

    private func receive(on c: NWConnection) {
        c.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, complete, error in
            guard let self, self.connection === c else { return }
            if let data, !data.isEmpty { self.onData?(data) }
            if complete || error != nil {
                c.cancel()
                return
            }
            self.receive(on: c)
        }
    }

    /// Must be called on `queue`.
    func send(_ data: Data) {
        guard let c = connection else { return }
        pendingBytes += data.count
        c.send(content: data, completion: .contentProcessed { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.pendingBytes = max(0, self.pendingBytes - data.count) }
        })
    }

    func disconnect() {
        connection?.cancel()
    }
}

import AppKit
import Foundation
import PingouinCore

/// The Mac's clipboard <-> a VM's (pingouind clipboard), over QEMU's
/// clipboard channel. Lines both ways: "T <base64 UTF-8 text>" (the clipboard
/// is now this); the guest sends "H" when its helper connects, and gets the
/// Mac's clipboard then. Nothing is sent at connect otherwise (starting a VM
/// never changes the Mac's clipboard), nothing echoes back, and passwords
/// (marked concealed by password managers) never go to the VM. Main thread.
@MainActor
final class ClipboardBridge {
    static let maxBytes = 1 << 20

    private let socketPath: String
    /// (Mac -> Linux, Linux -> Mac), read live: the switches can change while it runs.
    private let mode: () -> (toLinux: Bool, toMac: Bool)
    private var fd: Int32 = -1
    private var connectTimer: Timer?
    private var pollTimer: Timer?
    private var reader: DispatchSourceRead?
    private var inbox = Data()
    private var changeCount = NSPasteboard.general.changeCount
    private var lastSent: String?
    private var lastReceived: String?

    init(socketPath: String, mode: @escaping () -> (toLinux: Bool, toMac: Bool)) {
        self.socketPath = socketPath
        self.mode = mode
    }

    func start() {
        connectTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] t in
            Task { @MainActor in
                guard let self else { t.invalidate(); return }
                if self.connect() {
                    t.invalidate()
                    self.listen()
                }
            }
        }
    }

    func stop() {
        connectTimer?.invalidate()
        pollTimer?.invalidate()
        reader?.cancel()
        reader = nil
        if fd >= 0 { close(fd); fd = -1 }
    }

    private func connect() -> Bool {
        let s = socket(AF_UNIX, SOCK_STREAM, 0)
        guard s >= 0 else { return false }
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let ok = withUnsafeMutableBytes(of: &addr.sun_path) { buf -> Bool in
            let bytes = Array(socketPath.utf8)
            guard bytes.count < buf.count else { return false }
            buf.copyBytes(from: bytes)
            buf[bytes.count] = 0
            return true
        }
        let len = socklen_t(MemoryLayout<sockaddr_un>.size)
        let r = ok ? withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(s, $0, len) }
        } : -1
        guard r == 0 else { close(s); return false }
        var one: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        fd = s
        return true
    }

    private func listen() {
        let r = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
        r.setEventHandler { [weak self] in MainActor.assumeIsolated { self?.readLines() } }
        r.resume()
        reader = r
        // macOS has no "clipboard changed" notification: look twice a second.
        changeCount = NSPasteboard.general.changeCount
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func readLines() {
        var buf = [UInt8](repeating: 0, count: 65536)
        let n = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
        guard n > 0 else {
            if n == 0 { stop() }   // QEMU went away
            return
        }
        inbox.append(contentsOf: buf[0..<n])
        if inbox.count > Self.maxBytes * 2, inbox.firstIndex(of: UInt8(ascii: "\n")) == nil {
            inbox.removeAll()   // a line far too long: drop it
            return
        }
        while let nl = inbox.firstIndex(of: UInt8(ascii: "\n")) {
            let line = String(decoding: inbox[inbox.startIndex..<nl], as: UTF8.self)
            inbox = Data(inbox[(nl + 1)...])
            handle(line)
        }
    }

    private func handle(_ line: String) {
        if line == "H" {
            // the guest's helper is up: it gets what the Mac has now
            if mode().toLinux, let text = Self.shareableText() { send(text, force: true) }
        } else if line.hasPrefix("T "), mode().toMac,
                  let data = Data(base64Encoded: String(line.dropFirst(2))), data.count <= Self.maxBytes,
                  let text = String(data: data, encoding: .utf8), text != lastReceived {
            lastReceived = text
            lastSent = text   // don't send it back
            let pb = NSPasteboard.general
            pb.clearContents()
            pb.setString(text, forType: .string)
            changeCount = pb.changeCount
        }
    }

    private func poll() {
        let pb = NSPasteboard.general
        guard pb.changeCount != changeCount else { return }
        changeCount = pb.changeCount
        guard mode().toLinux, let text = Self.shareableText() else { return }
        send(text, force: false)
    }

    private func send(_ text: String, force: Bool) {
        guard fd >= 0, force || text != lastSent else { return }
        let data = Data(text.utf8)
        guard data.count <= Self.maxBytes else { return }
        lastSent = text
        var line = Data("T ".utf8)
        line.append(data.base64EncodedData())
        line.append(UInt8(ascii: "\n"))
        line.withUnsafeBytes { p in
            var off = 0
            while off < p.count {
                let w = write(fd, p.baseAddress! + off, p.count - off)
                if w <= 0 { break }
                off += w
            }
        }
    }

    /// Text on the Mac's clipboard, unless a password manager marked it as
    /// secret or temporary (nspasteboard.org's conventions).
    static func shareableText() -> String? {
        let pb = NSPasteboard.general
        let types = Set((pb.types ?? []).map(\.rawValue))
        let secret = ["org.nspasteboard.ConcealedType", "org.nspasteboard.TransientType",
                      "org.nspasteboard.AutoGeneratedType", "com.agilebits.onepassword"]
        if secret.contains(where: types.contains) { return nil }
        return pb.string(forType: .string)
    }
}

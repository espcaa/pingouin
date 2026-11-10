import AppKit
import ApplicationServices
import CPingouinGestures
import Foundation

/// Mac trackpad -> the VM: lines from the gestures library go into QEMU's
/// virtio-serial socket, and pingouind reads them in the guest.
/// Only one VM can hold the trackpad at a time (the library is global).
final class GesturesBridge {
    private let socketPath: String
    private let pid: pid_t
    private var fd: Int32 = -1
    private let lock = NSLock()
    private var connectTimer: Timer?
    private var started = false

    init(socketPath: String, qemuPID: pid_t) {
        self.socketPath = socketPath
        self.pid = qemuPID
    }

    static var hasAccessibility: Bool { pg_permissions() & Int32(PG_PERM_ACCESSIBILITY) != 0 }
    static var hasInputMonitoring: Bool { pg_permissions() & Int32(PG_PERM_INPUT_MONITORING) != 0 }
    static var hasPermissions: Bool { hasAccessibility && hasInputMonitoring }

    /// macOS's prompt (shown once per app), then the Settings pane: after a
    /// first "Deny", or a rebuild with a new signature, only the pane works.
    static func requestAccessibility() {
        let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary
        if !AXIsProcessTrustedWithOptions(opts) { openSettings("Privacy_Accessibility") }
    }

    static func requestInputMonitoring() {
        if !CGRequestListenEventAccess() { openSettings("Privacy_ListenEvent") }
    }

    private static func openSettings(_ pane: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") {
            NSWorkspace.shared.open(url)
        }
    }

    /// Connects once QEMU has made the socket (it does so right at start),
    /// then starts reading the trackpad.
    func start() {
        connectTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] t in
            guard let self else { t.invalidate(); return }
            if self.connect() {
                t.invalidate()
                self.startTrackpad()
            }
        }
    }

    func stop() {
        connectTimer?.invalidate()
        if started { pg_stop(); started = false }
        lock.lock()
        if fd >= 0 { close(fd); fd = -1 }
        lock.unlock()
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
        // Frames are disposable: never block the trackpad thread on a guest
        // that isn't reading (pingouind not running yet).
        _ = fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK)
        var one: Int32 = 1
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        lock.lock(); fd = s; lock.unlock()
        return true
    }

    private func startTrackpad() {
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        let rc = pg_start(pid, { line, len, ctx in
            guard let line, let ctx else { return }
            Unmanaged<GesturesBridge>.fromOpaque(ctx).takeUnretainedValue().send(line, len)
        }, ctx)
        started = rc == 0
        if started { pg_hello() }
    }

    private func send(_ line: UnsafePointer<CChar>, _ len: Int) {
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0 else { return }
        _ = write(fd, line, len) // EAGAIN: dropped, the next frame replaces it
    }
}

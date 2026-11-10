import AppKit
import ApplicationServices
import CPingouinGestures
import Foundation

/// Mac trackpad -> the VM: lines from the gestures library go into QEMU's
/// virtio-serial socket, and pingouind reads them in the guest.
/// Only one VM can hold the trackpad at a time (the library is global).
/// The channel is also how the app learns whether the guest tools are there:
/// it asks ("Q", every 30 s) with or without the permissions, and pingouind
/// answers "V <version>" then "D -" (versions before that: "D -" only).
final class GesturesBridge {
    private let socketPath: String
    private let pid: pid_t
    private var fd: Int32 = -1
    private let lock = NSLock()
    private var connectTimer: Timer?
    private var queryTimer: Timer?
    private var reader: DispatchSourceRead?
    private var inbox = Data()
    private var lastVersion: String?
    private var started = false
    private var hinted = false   // main thread: the ⌃⌥Esc hint, once per run
    /// Main thread: pingouind answered; its version, nil for tools from
    /// before versions were reported.
    var onTools: ((String?) -> Void)?

    init(socketPath: String, qemuPID: pid_t) {
        self.socketPath = socketPath
        self.pid = qemuPID
    }

    static func showPingouin() {
        NSApp.activate()
        if let w = NSApp.windows.first(where: { $0.canBecomeMain }) {
            if w.isMiniaturized { w.deminiaturize(nil) }
            w.makeKeyAndOrderFront(nil)
        }
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
    /// asks for the guest tools, and reads the trackpad if it may.
    func start() {
        connectTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] t in
            guard let self else { t.invalidate(); return }
            if self.connect() {
                t.invalidate()
                self.listen()
                self.startTrackpadIfAllowed()
            }
        }
    }

    /// The permissions came (or were there): the trackpad goes to the VM.
    func startTrackpadIfAllowed() {
        guard !started, fd >= 0, GesturesBridge.hasPermissions else { return }
        startTrackpad()
    }

    func stop() {
        connectTimer?.invalidate()
        queryTimer?.invalidate()
        reader?.cancel()
        reader = nil
        if started { pg_stop(); started = false }
        lock.lock()
        if fd >= 0 { close(fd); fd = -1 }
        lock.unlock()
    }

    /// Main thread: reads pingouind's lines, and asks until it answers (it
    /// may start long after QEMU), then every 30 s (tools installed meanwhile).
    private func listen() {
        let r = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
        r.setEventHandler { [weak self] in self?.readLines() }
        r.resume()
        reader = r
        query()
        queryTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in self?.query() }
    }

    private func query() {
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0 else { return }
        _ = write(fd, "Q\n", 2)
    }

    private func readLines() {
        var buf = [UInt8](repeating: 0, count: 1024)
        let n = buf.withUnsafeMutableBytes { read(fd, $0.baseAddress, $0.count) }
        guard n > 0 else { return }
        inbox.append(contentsOf: buf[0..<n])
        while let nl = inbox.firstIndex(of: UInt8(ascii: "\n")) {
            let line = String(decoding: inbox[inbox.startIndex..<nl], as: UTF8.self)
            inbox = Data(inbox[(nl + 1)...])
            if line.hasPrefix("V ") {
                lastVersion = String(line.dropFirst(2))
            } else if line == "D -" {
                onTools?(lastVersion)
                lastVersion = nil
                // answered: from now on only now and then
                if queryTimer?.timeInterval != 30 {
                    queryTimer?.invalidate()
                    queryTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
                        self?.query()
                    }
                }
            }
        }
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
        // Ctrl+Option+Esc let go of the VM ("S esc"): back to Pingouin's own
        // window (macOS switches Space). The VM captures again once it's in front.
        if len >= 5, strncmp(line, "S esc", 5) == 0 {
            DispatchQueue.main.async { EscapeHint.released(); GesturesBridge.showPingouin() }
        }
        // Capture went on: the VM is full screen and in front.
        if len >= 4, strncmp(line, "S on", 4) == 0 {
            DispatchQueue.main.async { [weak self] in
                guard let self, !self.hinted else { return }
                self.hinted = true
                EscapeHint.show()
            }
        }
        lock.lock()
        defer { lock.unlock() }
        guard fd >= 0 else { return }
        _ = write(fd, line, len) // EAGAIN: dropped, the next frame replaces it
    }
}

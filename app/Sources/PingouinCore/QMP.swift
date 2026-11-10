import Foundation

/// QEMU's control socket (QMP): one command per connection, blocking.
public enum QMP {
    public enum Failure: Error, CustomStringConvertible {
        case connect(String), io, refused(String)
        public var description: String {
            switch self {
            case .connect(let why): return "can't reach QEMU's control socket: \(why)"
            case .io: return "QEMU's control socket closed"
            case .refused(let msg): return "QEMU refused: \(msg)"
            }
        }
    }

    /// Runs `command` (e.g. "system_powerdown") and waits for QEMU's answer.
    public static func run(_ command: String, socket: URL) throws {
        let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw Failure.connect(String(cString: strerror(errno))) }
        defer { close(fd) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var tv = timeval(tv_sec: 5, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let path = Array(socket.path.utf8)
        guard path.count < MemoryLayout.size(ofValue: addr.sun_path) else { throw Failure.connect("path too long") }
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            buf.copyBytes(from: path)
            buf[path.count] = 0
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard ok == 0 else { throw Failure.connect(String(cString: strerror(errno))) }

        var pending = Data()
        // One JSON object per line; events ({"event": ...}) can come in between.
        func reply() throws -> [String: Any] {
            while true {
                if let nl = pending.firstIndex(of: UInt8(ascii: "\n")) {
                    let line = pending[pending.startIndex..<nl]
                    pending = Data(pending[(nl + 1)...])
                    guard let obj = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
                    if obj["event"] != nil { continue }
                    return obj
                }
                var buf = [UInt8](repeating: 0, count: 4096)
                let n = read(fd, &buf, buf.count)
                guard n > 0 else { throw Failure.io }
                pending.append(contentsOf: buf[0..<n])
            }
        }
        func send(_ cmd: String) throws {
            let line = Array("{\"execute\":\"\(cmd)\"}\n".utf8)
            guard write(fd, line, line.count) == line.count else { throw Failure.io }
            let r = try reply()
            if let e = r["error"] as? [String: Any] { throw Failure.refused("\(e["desc"] ?? e)") }
        }

        _ = try reply()   // the greeting
        try send("qmp_capabilities")
        try send(command)
    }
}

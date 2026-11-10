import Foundation

/// One VM's settings, stored as config.json in its folder.
public struct VMConfig: Codable, Equatable, Sendable {
    public var name: String
    public var cpus: Int
    public var memoryMiB: Int
    public var diskGiB: Int
    /// ISO keyboards (French, UK, German Macs...): QEMU swaps the § and < keys back.
    public var isoKeyboard: Bool
    /// Full screen over the notch strip too (the whole panel).
    public var fullPanel: Bool
    /// A Mac folder the guest sees as ~/Mac (virtio-9p, mount tag "pingouin").
    public var sharedFolder: String?

    public init(name: String, cpus: Int = VMConfig.defaultCPUs, memoryMiB: Int = VMConfig.defaultMemoryMiB,
                diskGiB: Int = 80, isoKeyboard: Bool = false, fullPanel: Bool = true,
                sharedFolder: String? = nil) {
        self.name = name
        self.cpus = cpus
        self.memoryMiB = memoryMiB
        self.diskGiB = diskGiB
        self.isoKeyboard = isoKeyboard
        self.fullPanel = fullPanel
        self.sharedFolder = sharedFolder
    }

    /// All cores but two: QEMU's GPU work (virgl) runs on the Mac side and needs some.
    public static var defaultCPUs: Int {
        max(2, ProcessInfo.processInfo.activeProcessorCount - 2)
    }

    /// Three quarters of the Mac's memory, in whole GiB, at least 4 GiB.
    /// The balloon hands unused guest memory back to macOS.
    public static var defaultMemoryMiB: Int {
        let gib = Int(ProcessInfo.processInfo.physicalMemory >> 30)
        return max(4, gib * 3 / 4) * 1024
    }
}

public enum VMError: Error, CustomStringConvertible {
    case invalidName(String)
    case exists(String)
    case notFound(String)
    case running(String)
    case runtimeMissing([String])

    public var description: String {
        switch self {
        case .invalidName(let n): return "invalid VM name \"\(n)\" (letters, digits, - and _ only)"
        case .exists(let n): return "a VM named \"\(n)\" already exists"
        case .notFound(let n): return "no VM named \"\(n)\""
        case .running(let n): return "\"\(n)\" is running; shut it down first"
        case .runtimeMissing(let tried): return "QEMU runtime not found; looked in:\n  " + tried.joined(separator: "\n  ")
        }
    }
}

/// A VM on disk: <root>/<name>/{config.json, disk.img, vars.fd}.
public struct VM: Sendable {
    public var config: VMConfig
    public let dir: URL

    public var diskURL: URL { dir.appendingPathComponent("disk.img") }
    public var varsURL: URL { dir.appendingPathComponent("vars.fd") }
    public var configURL: URL { dir.appendingPathComponent("config.json") }
    public var logURL: URL { dir.appendingPathComponent("qemu.log") }

    /// The gestures channel's socket. Unix socket paths max out at 104 bytes,
    /// so a long home folder falls back to a short path in /tmp.
    public var gesturesSocket: URL {
        let inDir = dir.appendingPathComponent("gestures.sock")
        if inDir.path.utf8.count < 100 { return inDir }
        return URL(fileURLWithPath: "/tmp/pingouin-\(getuid())-\(config.name).sock")
    }

    public func save() throws {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(config).write(to: configURL, options: .atomic)
    }
}

public struct VMStore: Sendable {
    public let root: URL

    /// ~/Library/Application Support/Pingouin/VMs, or $PINGOUIN_VMS.
    public static var `default`: VMStore {
        if let dir = ProcessInfo.processInfo.environment["PINGOUIN_VMS"] {
            return VMStore(root: URL(fileURLWithPath: dir, isDirectory: true))
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return VMStore(root: support.appendingPathComponent("Pingouin/VMs", isDirectory: true))
    }

    public init(root: URL) { self.root = root }

    public func list() -> [VM] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        return names.sorted().compactMap { try? load($0) }
    }

    public func load(_ name: String) throws -> VM {
        let dir = root.appendingPathComponent(name, isDirectory: true)
        guard let data = try? Data(contentsOf: dir.appendingPathComponent("config.json")) else {
            throw VMError.notFound(name)
        }
        return VM(config: try JSONDecoder().decode(VMConfig.self, from: data), dir: dir)
    }

    /// Makes the folder, a sparse disk and the firmware's variable store.
    public func create(_ config: VMConfig) throws -> VM {
        guard !config.name.isEmpty,
              config.name.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") })
        else { throw VMError.invalidName(config.name) }
        let dir = root.appendingPathComponent(config.name, isDirectory: true)
        guard !FileManager.default.fileExists(atPath: dir.path) else { throw VMError.exists(config.name) }
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let vm = VM(config: config, dir: dir)
        try sparseFile(vm.diskURL, bytes: UInt64(config.diskGiB) << 30)
        try sparseFile(vm.varsURL, bytes: 64 << 20) // the size QEMU's virt pflash expects
        try vm.save()
        return vm
    }

    /// Moves the VM's folder to the Trash (a symlinked VM: only the link).
    public func delete(_ vm: VM) throws {
        try FileManager.default.trashItem(at: vm.dir, resultingItemURL: nil)
    }

    private func sparseFile(_ url: URL, bytes: UInt64) throws {
        FileManager.default.createFile(atPath: url.path, contents: nil)
        let h = try FileHandle(forWritingTo: url)
        defer { try? h.close() }
        try h.truncate(atOffset: bytes)
    }
}

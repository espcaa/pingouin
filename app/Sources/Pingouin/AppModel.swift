import AppKit
import Carbon
import Foundation
import PingouinCore

/// A running VM: QEMU as a child process (so macOS counts it under
/// Pingouin.app's Accessibility grant) plus its gestures bridge.
final class RunningVM {
    enum Phase { case running, shuttingDown, restarting, forceStopping }

    let process: Process
    var gestures: GesturesBridge?
    var clipboard: ClipboardBridge?
    var phase = Phase.running

    init(process: Process, gestures: GesturesBridge?) {
        self.process = process
        self.gestures = gestures
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var vms: [VM] = []
    @Published var running: [String: RunningVM] = [:]
    /// What each running VM is doing (RunningVM.phase, published for the UI).
    @Published var phases: [String: RunningVM.Phase] = [:]
    @Published var error: String?
    @Published var creatingVM = false
    @Published var hasAccessibility = GesturesBridge.hasAccessibility
    @Published var hasInputMonitoring = GesturesBridge.hasInputMonitoring
    var hasGesturePermissions: Bool { hasAccessibility && hasInputMonitoring }

    let store = VMStore.default
    private var permissionTimer: Timer?

    init() {
        reload()
        // System Settings doesn't tell us when a switch flips: look every second.
        permissionTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refreshPermissions() }
        }
    }

    func reload() { vms = store.list() }

    func refreshPermissions() {
        let a = GesturesBridge.hasAccessibility, i = GesturesBridge.hasInputMonitoring
        if a != hasAccessibility { hasAccessibility = a }
        if i != hasInputMonitoring { hasInputMonitoring = i }
        if a && i { attachGestures() }
    }

    /// VMs started before the permissions were there get their trackpad now.
    private func attachGestures() {
        for r in running.values { r.gestures?.startTrackpadIfAllowed() }
    }

    // MARK: guest tools

    /// The guest tools this app carries (guest/build.sh writes VERSION).
    let bundledToolsVersion: String? = GuestTools.locate().flatMap {
        try? String(contentsOf: $0.appendingPathComponent("VERSION"), encoding: .utf8)
    }?.trimmingCharacters(in: .whitespacesAndNewlines)

    /// The guest tools a VM's pingouind last reported (kept in the VM's folder,
    /// so a stopped VM still shows it): "" = tools from before versions.
    @Published var seenTools: [String: String] = [:]
    /// Running VMs whose pingouind hasn't answered for a while (not installed, or not running).
    @Published var toolsSilent: Set<String> = []

    enum ToolsStatus: Equatable { case unknown, checking, missing, installed, outdated }

    func toolsStatus(_ vm: VM) -> ToolsStatus {
        let name = vm.config.name
        let seen = seenTools[name] ?? GuestToolsRecord.load(vm)
        if running[name] != nil, seen == nil {
            return toolsSilent.contains(name) ? .missing : .checking
        }
        guard let seen else { return .unknown }
        return seen == bundledToolsVersion ? .installed : .outdated
    }

    private func toolsAnswered(_ vm: VM, version: String?) {
        let v = version ?? ""
        toolsSilent.remove(vm.config.name)
        if seenTools[vm.config.name] != v {
            seenTools[vm.config.name] = v
            GuestToolsRecord.save(vm, version: v)
        }
    }

    /// Is the Mac's keyboard an ISO one (French, UK, German...)?
    static var macHasISOKeyboard: Bool {
        KBGetLayoutType(Int16(LMGetKbdType())) == kKeyboardISO
    }

    func create(_ config: VMConfig) -> VM? {
        do {
            let vm = try store.create(config)
            reload()
            return vm
        } catch {
            self.error = "\(error)"
            return nil
        }
    }

    func save(_ vm: VM) {
        do { try vm.save(); reload() } catch { self.error = "\(error)" }
    }

    /// Moves a stopped VM to the Trash.
    func delete(_ vm: VM) {
        guard running[vm.config.name] == nil else { error = "\(VMError.running(vm.config.name))"; return }
        do { try store.delete(vm); reload() } catch { self.error = "\(error)" }
    }

    func start(_ vm: VM, iso: URL? = nil) {
        guard running[vm.config.name] == nil else { return }
        do {
            let launch = QEMULaunch(vm: vm, runtime: try Runtime.locate(),
                                    iso: iso, icon: Bundle.main.url(forResource: "Pingouin", withExtension: "icns"),
                                    guestTools: GuestTools.locate())
            try? FileManager.default.removeItem(at: vm.gesturesSocket)
            try? FileManager.default.removeItem(at: vm.controlSocket)
            try? FileManager.default.removeItem(at: vm.displaySocket)
            try? FileManager.default.removeItem(at: vm.clipboardSocket)
            try? FileManager.default.removeItem(at: vm.displayNowURL)   // "now" is per run
            FileManager.default.createFile(atPath: vm.logURL.path, contents: Data((launch.shellLine + "\n\n").utf8))
            let log = try FileHandle(forWritingTo: vm.logURL)
            log.seekToEndOfFile()

            let p = Process()
            p.executableURL = launch.executable
            p.arguments = launch.arguments
            p.environment = ProcessInfo.processInfo.environment.merging(launch.environment) { $1 }
            p.standardOutput = log
            p.standardError = log
            let name = vm.config.name
            p.terminationHandler = { [weak self] proc in
                Task { @MainActor in self?.stopped(name, status: proc.terminationStatus) }
            }
            try p.run()

            // Always: it also tells whether the guest tools are there; the
            // trackpad only goes to the VM with the permissions.
            let gestures = GesturesBridge(socketPath: vm.gesturesSocket.path, qemuPID: p.processIdentifier)
            gestures.onTools = { [weak self] v in self?.toolsAnswered(vm, version: v) }
            gestures.start()
            seenTools[name] = nil
            toolsSilent.remove(name)
            // No answer after 2 minutes of running: say so (it keeps asking).
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(120))
                guard let self, self.running[name]?.process === p, self.seenTools[name] == nil else { return }
                self.toolsSilent.insert(name)
            }
            running[name] = RunningVM(process: p, gestures: gestures)
            // The clipboard: its mode is read live (it can change while running).
            let clipboard = ClipboardBridge(socketPath: vm.clipboardSocket.path) { [weak self] in
                let c = self?.vms.first(where: { $0.config.name == name })?.config
                return (c?.clipboardToLinux ?? false, c?.clipboardToMac ?? false)
            }
            clipboard.start()
            running[name]?.clipboard = clipboard
            phases[name] = .running
        } catch {
            self.error = "\(error)"
        }
    }

    /// Presses the VM's power button: Linux shuts down cleanly, then QEMU quits.
    func shutDown(_ vm: VM) { powerButton(vm, then: .shuttingDown) }

    /// Shuts down cleanly, then starts again (from the disk: an installer ISO
    /// is only for the run it was booted with).
    func restart(_ vm: VM) { powerButton(vm, then: .restarting) }

    /// Quits QEMU at once, like pulling the plug (for a VM that won't shut down).
    func forceStop(_ vm: VM) {
        guard let r = running[vm.config.name] else { return }
        set(r, vm.config.name, .forceStopping)
        r.process.terminate()
    }

    func shutDownAll() { for vm in vms where running[vm.config.name] != nil { shutDown(vm) } }
    func forceStopAll() { for vm in vms where running[vm.config.name] != nil { forceStop(vm) } }

    /// Resets the VM at once, like a reset button (no clean shutdown).
    func forceRestart(_ vm: VM) { qmp(vm, "system_reset") }

    private func powerButton(_ vm: VM, then phase: RunningVM.Phase) {
        guard let r = running[vm.config.name] else { return }
        set(r, vm.config.name, phase)
        qmp(vm, "system_powerdown")
    }

    private func set(_ r: RunningVM, _ name: String, _ phase: RunningVM.Phase) {
        r.phase = phase
        phases[name] = phase
    }

    private func qmp(_ vm: VM, _ command: String) {
        let socket = vm.controlSocket
        Task.detached {
            do { try QMP.run(command, socket: socket) } catch {
                await MainActor.run { self.error = "\(vm.config.name): \(error)" }
            }
        }
    }

    private func stopped(_ name: String, status: Int32) {
        let phase = running[name]?.phase
        running[name]?.gestures?.stop()
        running[name]?.clipboard?.stop()
        running[name] = nil
        phases[name] = nil
        guard let vm = vms.first(where: { $0.config.name == name }) else { return }
        if phase == .restarting {
            start(vm)
        } else if status != 0 && phase != .forceStopping {
            error = "\(name) stopped with status \(status). See \(vm.logURL.path)"
        }
    }
}

/// The last guest tools a VM reported, in its folder (guest-tools.json).
enum GuestToolsRecord {
    static func url(_ vm: VM) -> URL { vm.dir.appendingPathComponent("guest-tools.json") }

    static func load(_ vm: VM) -> String? {
        guard let d = try? Data(contentsOf: url(vm)),
              let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return nil }
        return o["version"] as? String
    }

    static func save(_ vm: VM, version: String) {
        let o: [String: Any] = ["version": version, "seen": ISO8601DateFormatter().string(from: Date())]
        if let d = try? JSONSerialization.data(withJSONObject: o, options: [.prettyPrinted, .sortedKeys]) {
            try? d.write(to: url(vm), options: .atomic)
        }
    }
}

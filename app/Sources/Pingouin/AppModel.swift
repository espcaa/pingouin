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
        for (name, r) in running where r.gestures == nil {
            guard let vm = vms.first(where: { $0.config.name == name }) else { continue }
            let g = GesturesBridge(socketPath: vm.gesturesSocket.path, qemuPID: r.process.processIdentifier)
            g.start()
            r.gestures = g
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

            var gestures: GesturesBridge?
            if GesturesBridge.hasPermissions {
                gestures = GesturesBridge(socketPath: vm.gesturesSocket.path, qemuPID: p.processIdentifier)
                gestures?.start()
            }
            running[name] = RunningVM(process: p, gestures: gestures)
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

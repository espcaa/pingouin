import AppKit
import Carbon
import Foundation
import PingouinCore

/// A running VM: QEMU as a child process (so macOS counts it under
/// Pingouin.app's Accessibility grant) plus its gestures bridge.
final class RunningVM {
    let process: Process
    let gestures: GesturesBridge?

    init(process: Process, gestures: GesturesBridge?) {
        self.process = process
        self.gestures = gestures
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var vms: [VM] = []
    @Published var running: [String: RunningVM] = [:]
    @Published var error: String?
    @Published var hasGesturePermissions = GesturesBridge.hasPermissions

    let store = VMStore.default

    init() { reload() }

    func reload() { vms = store.list() }

    func refreshPermissions() { hasGesturePermissions = GesturesBridge.hasPermissions }

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

    func start(_ vm: VM, iso: URL? = nil) {
        guard running[vm.config.name] == nil else { return }
        do {
            let launch = QEMULaunch(vm: vm, runtime: try Runtime.locate(),
                                    iso: iso, icon: Bundle.main.url(forResource: "Pingouin", withExtension: "icns"))
            try? FileManager.default.removeItem(at: vm.gesturesSocket)
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
        } catch {
            self.error = "\(error)"
        }
    }

    /// Asks the guest to shut down (ACPI power button), like closing QEMU's window.
    func stop(_ vm: VM) {
        running[vm.config.name]?.process.interrupt()
    }

    private func stopped(_ name: String, status: Int32) {
        running[name]?.gestures?.stop()
        running[name] = nil
        if status != 0, let vm = vms.first(where: { $0.config.name == name }) {
            error = "\(name) stopped with status \(status). See \(vm.logURL.path)"
        }
    }
}

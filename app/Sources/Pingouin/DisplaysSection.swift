import AppKit
import PingouinCore
import SwiftUI

/// The Mac's displays and whether a VM uses them in full screen, the same
/// choices ⌃⌥D makes (VM.displayChoicesURL, read live by QEMU).
@MainActor
final class DisplayChoices: ObservableObject {
    struct Display: Identifiable {
        let id: String      // the display's UUID
        let name: String
        let builtin: Bool
    }

    @Published var displays: [Display] = []
    @Published var choices: [String: Bool] = [:]
    private let url: URL
    private var timer: Timer?
    private var observer: NSObjectProtocol?

    init(vm: VM) {
        url = vm.displayChoicesURL
        refresh()
        observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in self?.refresh() } }
        // QEMU writes the file too (⌃⌥D)
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.loadChoices() }
        }
    }

    deinit {
        timer?.invalidate()
        if let observer { NotificationCenter.default.removeObserver(observer) }
    }

    func refresh() {
        displays = NSScreen.screens.compactMap { s in
            guard let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
                  let uuid = CGDisplayCreateUUIDFromDisplayID(n.uint32Value)?.takeRetainedValue()
            else { return nil }
            return Display(id: CFUUIDCreateString(nil, uuid) as String, name: s.localizedName,
                           builtin: CGDisplayIsBuiltin(n.uint32Value) != 0)
        }
        loadChoices()
    }

    func loadChoices() {
        let c = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode([String: Bool].self, from: $0) } ?? [:]
        if c != choices { choices = c }
    }

    func binding(_ d: Display) -> Binding<Bool> {
        Binding(get: { self.choices[d.id] == true }, set: { on in
            self.choices[d.id] = on
            let enc = JSONEncoder()
            enc.outputFormatting = [.prettyPrinted, .sortedKeys]
            try? enc.encode(self.choices).write(to: self.url, options: .atomic)
        })
    }
}

struct DisplaysSection: View {
    let vm: VM
    @StateObject private var model: DisplayChoices

    init(vm: VM) {
        self.vm = vm
        _model = StateObject(wrappedValue: DisplayChoices(vm: vm))
    }

    var body: some View {
        Section {
            if model.displays.count < 2 {
                Text("Plug in another display to give \(vm.config.name) a second screen.")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.displays) { d in
                    Toggle("\(d.name)\(d.builtin ? " (built-in)" : "")", isOn: model.binding(d))
                }
            }
        } header: {
            Text("Displays")
        } footer: {
            Text("In full screen, \(vm.config.name) also uses the displays switched on here (not the one its window is on). ⌃ + ⌥ + D toggles the display under the pointer. Needs the guest tools.")
                .font(.footnote).foregroundStyle(.secondary)
        }
    }
}

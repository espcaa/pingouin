import AppKit
import PingouinCore
import SwiftUI

/// The Mac's displays and whether a VM uses them in full screen, the same
/// choices ⌃⌥D makes (VM.displayChoicesURL, read live by QEMU).
@MainActor
final class DisplayChoices: ObservableObject {
    struct Display: Identifiable {
        let id: String  // the display's UUID
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

    /// Names of displays seen before, so a remembered one that isn't plugged
    /// in still shows by name (UUID -> name, for every VM).
    private static let namesKey = "displayNames"
    private var names: [String: String] {
        get { UserDefaults.standard.dictionary(forKey: Self.namesKey) as? [String: String] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: Self.namesKey) }
    }

    /// Choices for displays that aren't connected now.
    var remembered: [(id: String, name: String, use: Bool)] {
        let connected = Set(displays.map(\.id))
        return choices.filter { !connected.contains($0.key) }
            .map { (id: $0.key, name: names[$0.key] ?? "A display not connected", use: $0.value) }
            .sorted { $0.name < $1.name }
    }

    func refresh() {
        displays = NSScreen.screens.compactMap { s in
            guard
                let n = s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber,
                let uuid = CGDisplayCreateUUIDFromDisplayID(n.uint32Value)?.takeRetainedValue()
            else { return nil }
            return Display(
                id: CFUUIDCreateString(nil, uuid) as String, name: s.localizedName,
                builtin: CGDisplayIsBuiltin(n.uint32Value) != 0)
        }
        var n = names
        for d in displays where n[d.id] != d.name { n[d.id] = d.name }
        if n != names { names = n }
        loadChoices()
    }

    func loadChoices() {
        let c =
            (try? Data(contentsOf: url)).flatMap {
                try? JSONDecoder().decode([String: Bool].self, from: $0)
            } ?? [:]
        if c != choices { choices = c }
    }

    func binding(_ d: Display) -> Binding<Bool> {
        Binding(
            get: { self.choices[d.id] == true },
            set: { on in
                self.choices[d.id] = on
                self.save()
            })
    }

    /// Forgets a display: next time it's there, the VM asks again (the notice).
    func forget(_ id: String) {
        choices[id] = nil
        save()
    }

    private func save() {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? enc.encode(choices).write(to: url, options: .atomic)
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
            if model.remembered.isEmpty { explanation }
        }

        if !model.remembered.isEmpty {
            Section {
                ForEach(model.remembered, id: \.id) { r in
                    HStack {
                        Text(r.name)
                        Text(r.use ? "used" : "not used").foregroundStyle(.secondary)
                        Spacer()
                        Button(role: .destructive) {
                            model.forget(r.id)
                        } label: {
                            Image(systemName: "trash")
                        }
                        .help(
                            "Forget \(r.name): \(vm.config.name) asks again next time it's plugged in"
                        )
                    }
                }
            } header: {
                Text("Displays remembered")
            } footer: {
                explanation
            }
        }
    }

    private var explanation: some View {
        Text(
            "Forget a remembered display and it will ask again next time."
        )
        .font(.footnote).foregroundStyle(.secondary)
    }
}

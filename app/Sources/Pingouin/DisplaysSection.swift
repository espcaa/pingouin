import AppKit
import PingouinCore
import SwiftUI

/// The Mac's displays and how a VM uses them in full screen. Two files in the
/// VM's folder, both read live by QEMU (pingouin-displays-choice.patch,
/// pingouin-displays-now.patch) and written by ⌃⌥D too:
/// - displays.json, the rule: {"<display UUID>": true (auto connect) | false};
/// - displays-now.json, this run only: {"<UUID>": true | false}, emptied at
///   every start; a display missing there follows its rule.
@MainActor
final class DisplayChoices: ObservableObject {
    struct Display: Identifiable {
        let id: String  // the display's UUID
        let name: String
        let builtin: Bool
    }

    /// The Mac's other displays (the built-in one is the VM window's, not an extra).
    @Published var displays: [Display] = []
    @Published var choices: [String: Bool] = [:]
    @Published var now: [String: Bool] = [:]
    private let url: URL
    private let nowURL: URL
    private var timer: Timer?
    private var observer: NSObjectProtocol?

    init(vm: VM) {
        url = vm.displayChoicesURL
        nowURL = vm.displayNowURL
        refresh()
        observer = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in Task { @MainActor in self?.refresh() } }
        // QEMU writes the files too (⌃⌥D)
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.load() }
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

    /// Displays with a rule that aren't connected now.
    var remembered: [Display] {
        let connected = Set(displays.map(\.id))
        return choices.keys.filter { !connected.contains($0) }
            .map { Display(id: $0, name: names[$0] ?? "A display", builtin: false) }
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
        displays.removeAll(where: \.builtin)
        var n = names
        for d in displays where n[d.id] != d.name { n[d.id] = d.name }
        if n != names { names = n }
        load()
    }

    func load() {
        let c = Self.read(url)
        let w = Self.read(nowURL)
        if c != choices { choices = c }
        if w != now { now = w }
    }

    /// The rule: auto connect in full screen (a display never set: don't).
    func autoConnect(_ d: Display) -> Binding<Bool> {
        Binding(
            get: { self.choices[d.id] == true },
            set: { on in
                self.choices[d.id] = on
                Self.write(self.choices, self.url)
            })
    }

    /// This run: used now (else what its rule says).
    func usedNow(_ d: Display) -> Binding<Bool> {
        Binding(
            get: { self.now[d.id] ?? (self.choices[d.id] == true) },
            set: { on in
                self.now[d.id] = on
                Self.write(self.now, self.nowURL)
            })
    }

    /// Forgets a display's rule: next time it's there, the VM tells you (the notice).
    func forget(_ id: String) {
        choices[id] = nil
        Self.write(choices, url)
    }

    private static func read(_ url: URL) -> [String: Bool] {
        (try? Data(contentsOf: url)).flatMap {
            try? JSONDecoder().decode([String: Bool].self, from: $0)
        } ?? [:]
    }

    private static func write(_ d: [String: Bool], _ url: URL) {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try? enc.encode(d).write(to: url, options: .atomic)
    }
}

struct DisplaysSection: View {
    @EnvironmentObject var app: AppModel
    let vm: VM
    @StateObject private var model: DisplayChoices

    init(vm: VM) {
        self.vm = vm
        _model = StateObject(wrappedValue: DisplayChoices(vm: vm))
    }

    private var running: Bool { app.running[vm.config.name] != nil }

    var body: some View {
        Section {
            if model.displays.isEmpty && model.remembered.isEmpty {
                Text("Plug in another display to give \(vm.config.name) a second screen.")
                    .foregroundStyle(.secondary)
            }
            if !model.displays.isEmpty {
                ForEach(model.displays) { d in
                    HStack {
                        Text(d.name)
                        Spacer()
                        rulePicker(d)
                        if running {
                            Toggle("Use now", isOn: model.usedNow(d))
                                .toggleStyle(.switch).labelsHidden()
                                .help("Use \(d.name) for \(vm.config.name) now (this run only)")
                        }
                    }
                }
            }
            ForEach(model.remembered) { d in
                HStack {
                    Text(d.name)
                    Text("not connected").font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    rulePicker(d)
                    Button(role: .destructive) {
                        model.forget(d.id)
                    } label: {
                        Image(systemName: "trash")
                    }
                    .buttonStyle(.borderless)
                    .help("Forget \(d.name)")
                }
            }
        } header: {
            Text("Displays")
        } footer: {
            Text(
                "\(vm.config.name) can use your other displays too! Auto connect will use your display for linux every time you connect it. You can also press ⌃ + ⌥ + D on a display to easily toggle between windows/macos. Monitor arrangment is modified in macos, not in your de's settings."
            )
            .font(.footnote).foregroundStyle(.secondary)
        }
    }

    private func rulePicker(_ d: DisplayChoices.Display) -> some View {
        Picker("", selection: model.autoConnect(d)) {
            Text("Auto connect").tag(true)
            Text("Don't auto connect").tag(false)
        }
        .labelsHidden()
        .fixedSize()
    }
}

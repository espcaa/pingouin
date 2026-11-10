import PingouinCore
import SwiftUI
import UniformTypeIdentifiers

@main
struct PingouinApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    private var model: AppModel { delegate.model }

    var body: some Scene {
        // One window (no second copy with ⌘N); ⌘N makes a VM instead.
        Window("Pingouin", id: "main") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 640, minHeight: 400)
                .onReceive(
                    NotificationCenter.default.publisher(
                        for: NSApplication.didBecomeActiveNotification)
                ) { _ in
                    model.refreshPermissions()
                }
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New VM…") { model.creatingVM = true }.keyboardShortcut("n")
            }
        }

        Settings { SettingsView() }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var selection: String?
    @State private var deleting: VM?

    var body: some View {
        NavigationSplitView {
            List(model.vms, id: \.config.name, selection: $selection) { vm in
                HStack {
                    Text(vm.config.name)
                    Spacer()
                    if model.running[vm.config.name] != nil {
                        Circle().fill(.green).frame(width: 8, height: 8)
                    }
                }
                .contextMenu {
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([vm.dir])
                    }
                    Divider()
                    Button("Move to Trash…", role: .destructive) { deleting = vm }
                        .disabled(model.running[vm.config.name] != nil)
                }
            }
            .safeAreaInset(edge: .bottom) {
                Button {
                    model.creatingVM = true
                } label: {
                    Label("New VM", systemImage: "plus.circle")
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .padding(.horizontal, 12)
                .padding(.vertical, 10)
            }
        } detail: {
            VStack(spacing: 0) {
                if !model.hasGesturePermissions { PermissionBanner() }
                if let name = selection, let vm = model.vms.first(where: { $0.config.name == name })
                {
                    VMDetail(vm: vm, deleting: $deleting).id(name)
                } else {
                    ContentUnavailableView(
                        "No VM selected", systemImage: "macwindow",
                        description: Text("Make one with New VM, then boot a Linux installer ISO."))
                }
            }
        }
        .confirmationDialog(
            "Move \"\(deleting?.config.name ?? "")\" to the Trash?",
            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })
        ) {
            Button("Move to Trash", role: .destructive) {
                if let vm = deleting {
                    model.delete(vm)
                    if selection == vm.config.name { selection = model.vms.first?.config.name }
                }
                deleting = nil
            }
        } message: {
            Text("Its disk and settings go to the Trash; empty the Trash to free the space.")
        }
        .sheet(isPresented: $model.creatingVM) {
            NewVMSheet { vm in selection = vm.config.name }
        }
        .alert("Pingouin", isPresented: .constant(model.error != nil)) {
            Button("OK") { model.error = nil }
        } message: {
            Text(model.error ?? "")
        }
        .onAppear { if selection == nil { selection = model.vms.first?.config.name } }
    }
}

/// One row per permission still missing; each goes away once it's allowed.
struct PermissionBanner: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Trackpad gestures and ⌘ shortcuts in the VM need two permissions:")
                .font(.callout)
            if !model.hasAccessibility {
                row(
                    "Accessibility", "lets Pingouin pass ⌘ shortcuts and gestures to the VM",
                    GesturesBridge.requestAccessibility)
            }
            if !model.hasInputMonitoring {
                row(
                    "Input Monitoring", "lets Pingouin read the trackpad's fingers",
                    GesturesBridge.requestInputMonitoring)
            }
            Text(
                "Already on there? Remove Pingouin from that list with the [−] button and put it back again (you're probably on a new build with a new signature)."
            )
            .font(.caption).foregroundStyle(.secondary)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.yellow.opacity(0.15))
    }

    private func row(_ name: String, _ why: String, _ allow: @escaping () -> Void) -> some View {
        HStack {
            Image(systemName: "hand.raised")
            Text(name).bold() + Text(": \(why)")
            Spacer()
            Button("Allow \(name)…", action: allow)
        }
        .font(.callout)
    }
}

struct VMDetail: View {
    @EnvironmentObject var model: AppModel
    @State var vm: VM
    @Binding var deleting: VM?
    @State private var pickingISO = false

    var isRunning: Bool { model.running[vm.config.name] != nil }

    var body: some View {
        Form {
            Section {
                NumberRow(title: "CPUs", value: $vm.config.cpus, range: cpuRange)
                NumberRow(title: "Memory", value: gib($vm.config.memoryMiB), range: memoryGiBRange, unit: "GiB")
                Toggle("ISO keyboard (§ and < keys)", isOn: $vm.config.isoKeyboard)
                Toggle("Full screen over the notch", isOn: $vm.config.fullPanel)
                Toggle("Vulkan (needs Fedora's Mesa 26.2.4 or newer)", isOn: $vm.config.vulkan)
                LabeledContent("Disk", value: "\(vm.config.diskGiB) GiB (grows as used)")
            }
            .disabled(isRunning)
            .onChange(of: vm.config) { model.save(vm) }

            Section {
                LabeledContent("Shared folder") {
                    HStack {
                        if let f = vm.config.sharedFolder {
                            Text((f as NSString).abbreviatingWithTildeInPath)
                                .lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
                            Button("Remove") { vm.config.sharedFolder = nil }
                        } else {
                            Text("None").foregroundStyle(.secondary)
                        }
                        Button("Choose…", action: chooseFolder)
                    }
                }
                .disabled(isRunning)
            } footer: {
                Text("Shows up as ~/Mac inside Linux (after installing the guest tools below).")
                    .font(.footnote).foregroundStyle(.secondary)
            }

            Section("Guest tools") {
                VStack(alignment: .leading, spacing: 6) {
                    Text(
                        "Trackpad gestures, ⌘ shortcuts and the shared folder need Pingouin's tools in Linux. Once, in a terminal inside the VM:"
                    )
                    .font(.callout)
                    HStack {
                        Text(guestInstallCommand)
                            .font(.system(.callout, design: .monospaced)).textSelection(.enabled)
                        Spacer()
                        Button("Copy") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(guestInstallCommand, forType: .string)
                        }
                    }
                }
            }

            Section {
                HStack {
                    if isRunning {
                        switch model.phases[vm.config.name] {
                        case .shuttingDown?:
                            ProgressView().controlSize(.small)
                            Text("Shutting down…").foregroundStyle(.secondary)
                        case .restarting?:
                            ProgressView().controlSize(.small)
                            Text("Restarting…").foregroundStyle(.secondary)
                        default:
                            Button("Shut Down") { model.shutDown(vm) }
                            Button("Restart") { model.restart(vm) }
                        }
                        Menu("Force") {
                            Button("Force Restart") { model.forceRestart(vm) }
                            Button("Force Stop", role: .destructive) { model.forceStop(vm) }
                        }
                        .fixedSize()
                    } else {
                        Button("Start") { model.start(vm) }.keyboardShortcut(.defaultAction)
                        Button("Boot Installer ISO…") { pickingISO = true }
                    }
                    Spacer()
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([vm.dir])
                    }
                    Button("Move to Trash…", role: .destructive) { deleting = vm }.disabled(
                        isRunning)
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle(vm.config.name)
        .fileImporter(
            isPresented: $pickingISO,
            allowedContentTypes: [UTType(filenameExtension: "iso") ?? .data]
        ) { result in
            if case .success(let url) = result { model.start(vm, iso: url) }
        }
    }
}

let guestInstallCommand =
    "sudo mount -t 9p -o trans=virtio,ro pingouin-tools /mnt && sudo bash /mnt/install.sh; sudo umount /mnt"

extension VMDetail {
    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Share"
        panel.message = "The folder \(vm.config.name) sees as ~/Mac"
        if panel.runModal() == .OK, let url = panel.url { vm.config.sharedFolder = url.path }
    }
}

struct NewVMSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss
    var created: (VM) -> Void
    @State private var config = VMConfig(name: "fedora", isoKeyboard: AppModel.macHasISOKeyboard)

    var body: some View {
        Form {
            TextField("Name", text: $config.name)
            NumberRow(title: "CPUs", value: $config.cpus, range: cpuRange)
            NumberRow(title: "Memory", value: gib($config.memoryMiB), range: memoryGiBRange, unit: "GiB")
            NumberRow(title: "Disk", value: $config.diskGiB, range: 16...2048, step: 16, unit: "GiB",
                      slider: false)
            Toggle("ISO keyboard (§ and < keys)", isOn: $config.isoKeyboard)
        }
        .formStyle(.grouped)
        .frame(width: 420)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Create") {
                    if let vm = model.create(config) {
                        created(vm)
                        dismiss()
                    }
                }
            }
        }
    }
}

let cpuRange = 1...ProcessInfo.processInfo.activeProcessorCount
let memoryGiBRange = 2...max(2, Int(ProcessInfo.processInfo.physicalMemory >> 30))

/// A MiB setting shown and edited in whole GiB.
func gib(_ mib: Binding<Int>) -> Binding<Int> {
    Binding(get: { mib.wrappedValue / 1024 }, set: { mib.wrappedValue = $0 * 1024 })
}

/// A number you can drag (slider), type (field) or nudge (stepper);
/// whatever is typed is clamped to `range`.
struct NumberRow: View {
    let title: String
    @Binding var value: Int
    let range: ClosedRange<Int>
    var step = 1
    var unit = ""
    var slider = true

    private var clamped: Binding<Int> {
        Binding(get: { value }, set: { value = min(max($0, range.lowerBound), range.upperBound) })
    }

    var body: some View {
        LabeledContent(title) {
            HStack(spacing: 8) {
                if slider {
                    Slider(value: Binding(get: { Double(value) }, set: { clamped.wrappedValue = Int($0.rounded()) }),
                           in: Double(range.lowerBound)...Double(range.upperBound), step: Double(step))
                        .frame(minWidth: 180)
                }
                TextField(title, value: clamped, format: .number)
                    .labelsHidden()
                    .multilineTextAlignment(.trailing)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 60)
                Stepper(title, value: clamped, in: range, step: step).labelsHidden()
                if !unit.isEmpty { Text(unit).foregroundStyle(.secondary).frame(width: 28, alignment: .leading) }
            }
        }
    }
}

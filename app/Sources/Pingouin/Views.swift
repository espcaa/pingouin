import PingouinCore
import SwiftUI
import UniformTypeIdentifiers

@main
struct PingouinApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Pingouin") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 640, minHeight: 400)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
                    model.refreshPermissions()
                }
        }
        .windowResizability(.contentMinSize)
    }
}

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var selection: String?
    @State private var creating = false
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
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([vm.dir]) }
                    Divider()
                    Button("Move to Trash…", role: .destructive) { deleting = vm }
                        .disabled(model.running[vm.config.name] != nil)
                }
            }
            .safeAreaInset(edge: .bottom) {
                Button { creating = true } label: {
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
                if let name = selection, let vm = model.vms.first(where: { $0.config.name == name }) {
                    VMDetail(vm: vm, deleting: $deleting).id(name)
                } else {
                    ContentUnavailableView("No VM selected", systemImage: "macwindow",
                                           description: Text("Make one with New VM, then boot a Linux installer ISO."))
                }
            }
        }
        .confirmationDialog("Move \"\(deleting?.config.name ?? "")\" to the Trash?",
                            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
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
        .sheet(isPresented: $creating) {
            NewVMSheet { vm in selection = vm.config.name }
        }
        .alert("Pingouin", isPresented: .constant(model.error != nil)) {
            Button("OK") { model.error = nil }
        } message: { Text(model.error ?? "") }
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
                row("Accessibility", "lets Pingouin pass ⌘ shortcuts and gestures to the VM",
                    GesturesBridge.requestAccessibility)
            }
            if !model.hasInputMonitoring {
                row("Input Monitoring", "lets Pingouin read the trackpad's fingers",
                    GesturesBridge.requestInputMonitoring)
            }
            Text("Already on in System Settings? Remove Pingouin from that list with − and allow it again (a new build has a new signature).")
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
                Stepper("CPUs: \(vm.config.cpus)", value: $vm.config.cpus,
                        in: 1...ProcessInfo.processInfo.activeProcessorCount)
                Stepper("Memory: \(vm.config.memoryMiB / 1024) GiB", value: $vm.config.memoryMiB,
                        in: 2048...Int(ProcessInfo.processInfo.physicalMemory >> 20), step: 1024)
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
                    Text("Trackpad gestures, ⌘ shortcuts and the shared folder need Pingouin's tools in Linux. Once, in a terminal inside the VM:")
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
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([vm.dir]) }
                    Button("Move to Trash…", role: .destructive) { deleting = vm }.disabled(isRunning)
                }
            } footer: {
                Text("After installing, shut the VM down (don't reboot) and press Start: the ISO is only attached for the run you boot it with.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .navigationTitle(vm.config.name)
        .fileImporter(isPresented: $pickingISO, allowedContentTypes: [UTType(filenameExtension: "iso") ?? .data]) { result in
            if case .success(let url) = result { model.start(vm, iso: url) }
        }
    }
}

let guestInstallCommand = "sudo mount -t 9p -o trans=virtio,ro pingouin-tools /mnt && sudo bash /mnt/install.sh; sudo umount /mnt"

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
            Stepper("CPUs: \(config.cpus)", value: $config.cpus, in: 1...ProcessInfo.processInfo.activeProcessorCount)
            Stepper("Memory: \(config.memoryMiB / 1024) GiB", value: $config.memoryMiB,
                    in: 2048...Int(ProcessInfo.processInfo.physicalMemory >> 20), step: 1024)
            Stepper("Disk: \(config.diskGiB) GiB", value: $config.diskGiB, in: 16...2048, step: 16)
            Toggle("ISO keyboard (§ and < keys)", isOn: $config.isoKeyboard)
        }
        .formStyle(.grouped)
        .frame(width: 420)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Create") {
                    if let vm = model.create(config) { created(vm); dismiss() }
                }
            }
        }
    }
}

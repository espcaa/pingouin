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
            }
            .toolbar {
                Button { creating = true } label: { Label("New VM", systemImage: "plus") }
            }
        } detail: {
            VStack(spacing: 0) {
                if !model.hasGesturePermissions { PermissionBanner() }
                if let name = selection, let vm = model.vms.first(where: { $0.config.name == name }) {
                    VMDetail(vm: vm).id(name)
                } else {
                    ContentUnavailableView("No VM selected", systemImage: "macwindow",
                                           description: Text("Make one with + and boot a Linux installer ISO."))
                }
            }
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

struct PermissionBanner: View {
    var body: some View {
        HStack {
            Image(systemName: "hand.raised")
            Text("Trackpad gestures and ⌘ shortcuts in the VM need Accessibility and Input Monitoring.")
                .font(.callout)
            Spacer()
            Button("Allow…") { GesturesBridge.requestPermissions() }
        }
        .padding(10)
        .background(.yellow.opacity(0.15))
    }
}

struct VMDetail: View {
    @EnvironmentObject var model: AppModel
    @State var vm: VM
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
                LabeledContent("Disk", value: "\(vm.config.diskGiB) GiB (grows as used)")
            }
            .disabled(isRunning)
            .onChange(of: vm.config) { model.save(vm) }

            Section {
                HStack {
                    if isRunning {
                        Button("Shut Down") { model.stop(vm) }
                    } else {
                        Button("Start") { model.start(vm) }.keyboardShortcut(.defaultAction)
                        Button("Boot Installer ISO…") { pickingISO = true }
                    }
                    Spacer()
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([vm.dir]) }
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

import AppKit
import PingouinCore
import SwiftUI

/// Whether the VM has Pingouin's guest tools, and how to install or update them.
struct GuestToolsSection: View {
    @EnvironmentObject var model: AppModel
    let vm: VM

    var body: some View {
        let status = model.toolsStatus(vm)
        Section {
            HStack(spacing: 8) {
                icon(status)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title(status))
                    Text(detail(status)).font(.caption).foregroundStyle(.secondary)
                }
            }
            if status != .installed {
                VStack(alignment: .leading, spacing: 6) {
                    Text(status == .outdated ? "To update, run this in a terminal inside the VM:"
                                             : "To install, run this in a terminal inside the VM:")
                        .font(.callout)
                    command
                }
            } else {
                DisclosureGroup("Reinstall") { command }.font(.callout)
            }
        } header: {
            Text("Guest tools")
        }
    }

    private var command: some View {
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

    @ViewBuilder private func icon(_ s: AppModel.ToolsStatus) -> some View {
        switch s {
        case .checking: ProgressView().controlSize(.small)
        case .installed: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .outdated: Image(systemName: "arrow.triangle.2.circlepath.circle.fill").foregroundStyle(.orange)
        case .missing: Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.orange)
        case .unknown: Image(systemName: "questionmark.circle").foregroundStyle(.secondary)
        }
    }

    private func title(_ s: AppModel.ToolsStatus) -> String {
        switch s {
        case .checking: return "Looking for the guest tools…"
        case .installed: return "Installed"
        case .outdated: return "Outdated"
        case .missing: return "Not found"
        case .unknown: return "Not checked yet"
        }
    }

    private func detail(_ s: AppModel.ToolsStatus) -> String {
        switch s {
        case .checking: return "Waiting for Linux to start."
        case .installed: return "Gestures, ⌘ shortcuts, extra displays and the shared folder work."
        case .outdated: return "This version of Pingouin has newer ones."
        case .missing: return "Gestures, ⌘ shortcuts, extra displays and the shared folder need them."
        case .unknown: return "Start \(vm.config.name) to check."
        }
    }
}

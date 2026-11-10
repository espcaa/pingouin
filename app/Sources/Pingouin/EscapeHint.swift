import AppKit
import SwiftUI

/// "Press ⌃⌥Esc to get back to your Mac": a small notice over a VM that just
/// went full screen, the first time each run. It doesn't take focus: the VM
/// keeps the keyboard, the button still takes a click.
@MainActor
enum EscapeHint {
    static let defaultsKey = "showEscapeHint"
    static var enabled: Bool {
        get { UserDefaults.standard.object(forKey: defaultsKey) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: defaultsKey) }
    }

    private static var panel: NSPanel?
    private static var hideTask: Task<Void, Never>?

    static func show() {
        guard enabled, panel == nil, let screen = NSScreen.main else { return }
        let view = NSHostingView(rootView: HintView(dismiss: { hide() }))
        let size = view.fittingSize
        let f = screen.frame
        // Below the notch strip (full-panel mode draws there).
        let origin = NSPoint(x: f.midX - size.width / 2, y: f.maxY - size.height - 60)
        let p = NSPanel(contentRect: NSRect(origin: origin, size: size),
                        styleMask: [.nonactivatingPanel, .borderless], backing: .buffered, defer: false)
        p.contentView = view
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = true
        p.level = .statusBar
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
        p.alphaValue = 0
        p.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { $0.duration = 0.25; p.animator().alphaValue = 1 }
        panel = p
        hideTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(20))
            if !Task.isCancelled { hide() }
        }
    }

    static func hide() {
        hideTask?.cancel()
        guard let p = panel else { return }
        panel = nil
        NSAnimationContext.runAnimationGroup({ $0.duration = 0.3; p.animator().alphaValue = 0 },
                                             completionHandler: { p.orderOut(nil) })
    }
}

private struct HintView: View {
    var dismiss: () -> Void

    var body: some View {
        HStack(spacing: 14) {
            (Text("Press ") + Text("⌃ + ⌥ + Esc").bold()
             + Text(" to let go of the VM, then swipe with three fingers to get back to your desktop"))
                .font(.system(size: 15))
            Button(action: dismiss) { Image(systemName: "xmark") }
                .buttonStyle(.borderless).controlSize(.small)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .background(.regularMaterial, in: Capsule())
        .environment(\.colorScheme, .dark)
        .fixedSize()
    }
}

struct SettingsView: View {
    @AppStorage(EscapeHint.defaultsKey) private var showEscapeHint = true

    var body: some View {
        Form {
            Toggle("Show the ⌃ + ⌥ + Esc hint when a VM goes full screen", isOn: $showEscapeHint)
        }
        .formStyle(.grouped)
        .frame(width: 440)
    }
}

import AppKit

/// Quitting Pingouin shuts its VMs down first: a QEMU left running would keep
/// grabbing the keyboard with nothing in charge of it.
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel()
    private var waiting: Timer?

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let names = model.running.keys.sorted()
        guard !names.isEmpty else { return .terminateNow }

        let alert = NSAlert()
        alert.messageText = names.count == 1 ? "\(names[0]) is running" : "\(names.count) VMs are running"
        alert.informativeText = "Pingouin shuts \(names.count == 1 ? "it" : "them") down (like the power button) before quitting."
        alert.addButton(withTitle: "Shut Down and Quit")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return .terminateCancel }

        model.shutDownAll()
        let deadline = Date().addingTimeInterval(30)
        waiting = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] t in
            Task { @MainActor in
                guard let self else { return }
                if self.model.running.isEmpty {
                    t.invalidate()
                    NSApp.reply(toApplicationShouldTerminate: true)
                } else if Date() > deadline {
                    // Linux didn't shut down in 30 s: pull the plug.
                    self.model.forceStopAll()
                }
            }
        }
        return .terminateLater
    }
}

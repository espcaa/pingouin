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

        // Quit as soon as the last VM has stopped.
        model.onAllStopped = { [weak self] in
            self?.waiting?.invalidate()
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        model.shutDownAll()
        // Linux didn't shut down in 30 s: pull the plug. While AppKit waits
        // (.terminateLater) its run loop is in the modal panel mode: a timer in
        // the default mode never fired, and Pingouin never quit.
        let t = Timer(timeInterval: 30, repeats: false) { [weak self] _ in
            Task { @MainActor in self?.model.forceStopAll() }
        }
        RunLoop.main.add(t, forMode: .common)
        waiting = t
        return .terminateLater
    }
}

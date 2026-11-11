import Sparkle
import SwiftUI

/// Sparkle: checks github.com/espcaa/pingouin's releases (SUFeedURL, which only
/// release builds have: scripts/bundle.sh RELEASE=1). Dev builds have no updater.
@MainActor
final class Updates: ObservableObject {
    static let shared = Updates()
    private let controller: SPUStandardUpdaterController?
    @Published var canCheck = false

    private init() {
        guard Bundle.main.object(forInfoDictionaryKey: "SUFeedURL") != nil else {
            controller = nil
            return
        }
        let c = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        controller = c
        c.updater.publisher(for: \.canCheckForUpdates).assign(to: &$canCheck)
    }

    var available: Bool { controller != nil }

    func check() { controller?.checkForUpdates(nil) }
}

/// "Check for Updates…" under the app's About item (release builds only).
struct UpdateCommands: Commands {
    @ObservedObject var updates = Updates.shared

    var body: some Commands {
        CommandGroup(after: .appInfo) {
            if updates.available {
                Button("Check for Updates…") { updates.check() }
                    .disabled(!updates.canCheck)
            }
        }
    }
}

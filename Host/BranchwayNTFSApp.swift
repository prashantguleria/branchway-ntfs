// Branchway NTFS.app: the companion app that carries BranchwayNTFSModule.appex (Contents/Extensions).
// It does three things: shows whether macOS sees the module and whether it is on, opens the System
// Settings switch that turns it on, and shows the version. It never mounts anything.
import AppKit
import FSKit
import SwiftUI

@main
struct BranchwayNTFSApp: App {
    var body: some Scene {
        Window("Branchway NTFS", id: "main") {
            StatusView()
        }
        .windowResizability(.contentSize)
    }
}

/// The module's bundle identifier (project.yml, target BranchwayNTFSModule).
let moduleBundleID = "com.evenpine.branchwayntfs.module"

enum ModuleState: Equatable {
    case checking
    case notFound          // macOS does not list the module (not installed, or app not in /Applications yet)
    case off               // listed, switched off in System Settings
    case on
    case failed(String)

    var title: String {
        switch self {
        case .checking: "Checking…"
        case .notFound: "Not found by macOS"
        case .off: "Installed · Off"
        case .on: "On"
        case .failed: "Couldn’t check"
        }
    }

    var detail: String {
        switch self {
        case .checking: ""
        case .notFound: "Move Branchway NTFS to Applications and open it once."
        case .off: "Turn on Branchway NTFS in System Settings › General › Login Items & Extensions › File System Extensions."
        case .on: "NTFS drives you plug in open with Branchway NTFS. New drives are Read Only until you choose Read & Write in Branchway."
        case .failed(let message): message
        }
    }
}

@MainActor
final class ModuleStatus: ObservableObject {
    @Published var state: ModuleState = .checking

    /// FSClient.h:33 `fetchInstalledExtensionsWithCompletionHandler:` (V1, macOS 15.4), as `installedExtensions`.
    func refresh() async {
        do {
            let modules = try await FSClient.shared.installedExtensions
            if let module = modules.first(where: { $0.bundleIdentifier == moduleBundleID }) {
                state = module.isEnabled ? .on : .off
            } else {
                state = .notFound
            }
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func openSettings() {
        if #available(macOS 27.0, *) {
            // FSClient.h:61 `openFileSystemExtensionsSettings` (V3, macOS 27.0)
            if FSClient.shared.openFileSystemExtensionsSettings() { return }
        }
        // macOS 15.4–26: no API. Open the Login Items & Extensions pane.
        // UNVERIFIED until ADP: this pane URL on 15.4–26, and whether its switch enables a third-party module.
        if let url = URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }
}

struct StatusView: View {
    @StateObject private var status = ModuleStatus()

    private var version: String {
        let info = Bundle.main.infoDictionary
        let short = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "Version \(short) (\(build))"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Branchway NTFS").font(.title2.weight(.semibold))
            HStack(spacing: 8) {
                Circle().fill(status.state == .on ? Color.green : Color.secondary).frame(width: 8, height: 8)
                Text(status.state.title).font(.headline)
            }
            if !status.state.detail.isEmpty {
                Text(status.state.detail).font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Button("Open System Settings") { status.openSettings() }
                    .disabled(status.state == .on)
                Spacer()
                Text(version).font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(20)
        .frame(width: 420)
        .task { await status.refresh() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            Task { await status.refresh() }
        }
    }
}

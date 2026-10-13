// Per-volume write policy and the status the module reports back, both in the app group's
// shared defaults (suite = Info.plist BWAppGroup, "$(TeamIdentifierPrefix)com.evenpine.drives").
//
// The Branchway app (NTFS-2b) writes the policy; the module only reads it, and writes the
// status of each volume it loads so the app can show why a drive is read-only.
// UNVERIFIED until ADP: a sandboxed FSKit appex reading/writing its app group's defaults
// (needs the signed com.apple.security.application-groups entitlement; unsigned builds have none).
import Foundation

enum DrivePolicy: String, Sendable {
    case readOnly
    case readWrite
}

struct PolicyStore: @unchecked Sendable {
    /// Info.plist key that carries the app group identifier (Module/Info.plist).
    static let appGroupInfoKey = "BWAppGroup"
    /// The fallback when a drive has no policy of its own; itself defaults to read-only.
    static let defaultKey = "ntfs.policy.default"

    let defaults: UserDefaults?

    init(defaults: UserDefaults?) { self.defaults = defaults }

    /// The app group store named in the bundle's Info.plist, nil when unset or unavailable.
    static func appGroup(bundle: Bundle = .main) -> PolicyStore {
        guard let group = bundle.object(forInfoDictionaryKey: appGroupInfoKey) as? String,
              !group.isEmpty, !group.hasPrefix("$(") else { return PolicyStore(defaults: nil) }
        return PolicyStore(defaults: UserDefaults(suiteName: group))
    }

    /// The per-volume key: the NTFS volume serial number as 16 hex digits.
    static func policyKey(serial: UInt64) -> String { String(format: "ntfs.policy.%016llX", serial) }
    static func statusKey(serial: UInt64) -> String { String(format: "ntfs.status.%016llX", serial) }

    /// Read-only unless the app set read-write for this drive (or as the default).
    func policy(serial: UInt64) -> DrivePolicy {
        guard let defaults else { return .readOnly }
        if let s = defaults.string(forKey: Self.policyKey(serial: serial)), let p = DrivePolicy(rawValue: s) { return p }
        if let s = defaults.string(forKey: Self.defaultKey), let p = DrivePolicy(rawValue: s) { return p }
        return .readOnly
    }

    /// The writer the app uses (NTFS-2b spec); here for the tests and as the reference.
    func setPolicy(_ policy: DrivePolicy, serial: UInt64) {
        defaults?.set(policy.rawValue, forKey: Self.policyKey(serial: serial))
    }

    /// What the module tells the app about a loaded volume.
    func writeStatus(serial: UInt64, label: String, readOnly: Bool, reasons: ReadOnlyReasons, failure: Int32) {
        defaults?.set([
            "label": label,
            "readOnly": readOnly,
            "reasons": Int(reasons.rawValue),
            "explanations": reasons.explanations,
            "failure": Int(failure),
            "updated": Date().timeIntervalSince1970,
        ] as [String: Any], forKey: Self.statusKey(serial: serial))
    }
}

/// Options fskitd passes to loadResource: `-f` force and `--rdonly`
/// (FSUnaryFileSystem.h:44: "An FSUnaryFileSystem supports two options").
struct LoadOptions: Sendable, Equatable {
    var readOnly = false
    var force = false

    init(_ options: [String]) {
        for o in options {
            switch o {
            case "--rdonly", "-r", "rdonly", "ro": readOnly = true
            case "-f", "--force": force = true
            default:
                // "-o rdonly" style pairs and comma lists
                if o.split(separator: ",").contains(where: { $0 == "rdonly" || $0 == "ro" }) { readOnly = true }
            }
        }
    }
}

/// fsck options (Info.plist FSCheckOptionSyntax shortOptions "nqy"): -q quick (flags and journal
/// state only, never writes), -y repair (replay the journal, then walk), -n check only.
/// -q wins over -y: a quick check never writes.
struct CheckOptions: Sendable, Equatable {
    var mode: CheckMode

    init(_ options: [String]) {
        var quick = false, repair = false
        for o in options where o.hasPrefix("-") && !o.hasPrefix("--") {
            if o.contains("q") { quick = true }
            if o.contains("y") { repair = true }
            if o.contains("n") { repair = false }
        }
        mode = quick ? .quick : repair ? .repair : .check
    }
}

/// Group commit interval (NTFS-2 brief): 5 s on removable media, 1 s on fixed.
enum CommitInterval {
    static func milliseconds(removable: Bool) -> Int { removable ? 5000 : 1000 }
}

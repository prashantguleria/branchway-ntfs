// Unified logging for the module, and the sink for libntfs-3g / engine messages (bwntfs.h:38).
import BWNTFSEngine
import FSKit
import os

enum ModuleLog {
    static let fs = Logger(subsystem: "com.evenpine.branchwayntfs", category: "fs")
    static let engine = Logger(subsystem: "com.evenpine.branchwayntfs", category: "engine")

    /// Routes engine messages to the unified log. Idempotent.
    static func install() {
        bw_set_log { level, msg in
            guard let msg else { return }
            let s = String(cString: msg)
            switch level {
            case 2: ModuleLog.engine.error("\(s, privacy: .public)")
            case 1: ModuleLog.engine.warning("\(s, privacy: .public)")
            default: ModuleLog.engine.info("\(s, privacy: .public)")
            }
        }
    }
}

/// POSIX errno -> the NSError FSKit expects (FSKitFunctions: fs_errorForPOSIXError).
func posixError(_ code: Int32) -> any Error { fs_errorForPOSIXError(code) }

func fsError(_ error: any Error) -> any Error {
    if let e = error as? EngineError { return posixError(e.code) }
    return error
}

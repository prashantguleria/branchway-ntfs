// A Swift face on the engine API (Engine/include/bwntfs.h). Every method runs on
// EngineQueue.queue (bwntfs.h:7-8) and throws EngineError (a positive errno).
import BWNTFSEngine
import Darwin
import Foundation

/// A C char array (fixed-size tuple) as a String.
func cString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        let bytes = raw.bindMemory(to: UInt8.self)
        let n = bytes.firstIndex(of: 0) ?? bytes.count
        return String(decoding: bytes[..<n], as: UTF8.self)
    }
}

/// What a probe found (bwntfs.h:41-52).
struct ProbeInfo: Sendable, Equatable {
    enum Kind: Sendable, Equatable { case notRecognized, ntfs, bitLocker, refs }
    var kind: Kind
    var label: String
    var serial: UInt64
    var dirty: Bool
    var majorVersion: Int
    var minorVersion: Int
    var clusterSize: UInt32
    var totalBytes: UInt64

    /// A stable container/volume UUID from the NTFS volume serial (NTFS has no UUID).
    /// Bytes 0-7: "BWNTFS\0\0" marker; bytes 8-15: the serial, big-endian; version/variant bits set
    /// so it is a well-formed (version 8, custom) UUID.
    var containerUUID: UUID {
        var b: [UInt8] = Array("BWNTFS".utf8) + [0, 0]
        for i in (0..<8).reversed() { b.append(UInt8(truncatingIfNeeded: serial >> (UInt64(i) * 8))) }
        b[6] = (b[6] & 0x0F) | 0x80
        b[8] = (b[8] & 0x3F) | 0x80
        return UUID(uuid: (b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]))
    }
}

/// Why a volume is read-only (bwntfs.h:63-70).
struct ReadOnlyReasons: OptionSet, Sendable, Hashable {
    let rawValue: Int32
    static let policy = ReadOnlyReasons(rawValue: Int32(BW_RO_POLICY))
    static let dirty = ReadOnlyReasons(rawValue: Int32(BW_RO_DIRTY))
    static let hibernated = ReadOnlyReasons(rawValue: Int32(BW_RO_HIBERNATED))
    static let journal = ReadOnlyReasons(rawValue: Int32(BW_RO_JOURNAL))
    static let device = ReadOnlyReasons(rawValue: Int32(BW_RO_DEVICE))
    static let failed = ReadOnlyReasons(rawValue: Int32(BW_RO_FAILED))

    /// One plain sentence per reason, for the log and the app (NTFS-2b reads them back).
    var explanations: [String] {
        var s: [String] = []
        if contains(.dirty) { s.append("Windows did not shut this drive down cleanly. Run chkdsk on Windows to write to it.") }
        if contains(.hibernated) { s.append("Windows is hibernated or uses Fast Startup on this drive. Shut Windows down fully to write to it.") }
        if contains(.journal) { s.append("The write journal could not be set up (the drive may be full).") }
        if contains(.device) { s.append("The drive itself is read-only.") }
        if contains(.failed) { s.append("A write to the drive failed, so the drive was switched to read-only.") }
        if contains(.policy) && s.isEmpty { s.append("Read-only is the setting for this drive.") }
        return s
    }
}

struct MountResult: Sendable {
    var readOnly: Bool
    var reasons: ReadOnlyReasons
    var journalEvent: Int32
    var replayedTransactions: UInt64
    var replayedBlocks: UInt64
    var stale: Bool
}

struct VolumeStats: Sendable {
    var clusterSize: UInt32
    var totalClusters: UInt64
    var freeClusters: UInt64
    var totalFiles: UInt64
    var freeFiles: UInt64
    var label: String
    var serial: UInt64
}

/// bw_attr in Swift (bwntfs.h:96-110).
struct ItemAttributes: Sendable, Equatable {
    enum Kind: Int32, Sendable { case file = 1, directory = 2, symlink = 3 }
    var ref: UInt64
    var parentRef: UInt64
    var kind: Kind
    var mode: UInt32
    var linkCount: UInt32
    var flags: UInt32
    var ntfsAttributes: UInt32
    var size: UInt64
    var allocSize: UInt64
    var mtime: timespec
    var ctime: timespec
    var atime: timespec
    var btime: timespec

    static func == (a: ItemAttributes, b: ItemAttributes) -> Bool {
        a.ref == b.ref && a.size == b.size && a.flags == b.flags && a.kind == b.kind && a.mode == b.mode
    }

    init(_ a: bw_attr) {
        ref = a.ref
        parentRef = a.parent_ref
        kind = Kind(rawValue: a.type) ?? .file
        mode = a.mode
        linkCount = a.nlink
        flags = a.flags
        ntfsAttributes = a.ntfs_attrib
        size = a.size
        allocSize = a.alloc_size
        func ts(_ t: bw_time) -> timespec { timespec(tv_sec: Int(t.sec), tv_nsec: Int(t.nsec)) }
        mtime = ts(a.mtime)
        ctime = ts(a.ctime)
        atime = ts(a.atime)
        btime = ts(a.btime)
    }

    var isHidden: Bool { flags & UInt32(BW_F_HIDDEN) != 0 }
    var isReadOnlyFlag: Bool { flags & UInt32(BW_F_READONLY) != 0 }
    /// resident, compressed, encrypted or sparse data: no kernel-offloaded I/O (bwntfs.h:93)
    var inhibitsKernelIO: Bool { flags & UInt32(BW_F_NO_KOIO) != 0 }
}

struct DirectoryEntry: Sendable {
    var name: String
    var kind: ItemAttributes.Kind
    var ref: UInt64
    /// the engine cookie that continues after this entry
    var nextCookie: UInt64
}

struct Extent: Sendable, Equatable {
    var logical: Int64
    var physical: Int64
    var length: Int64
    var zeroFill: Bool
}

enum CheckMode: Sendable, Equatable {
    case quick, check, repair
    var raw: Int32 {
        switch self {
        case .quick: Int32(BW_FSCK_QUICK)
        case .check: Int32(BW_FSCK_CHECK)
        case .repair: Int32(BW_FSCK_REPAIR)
        }
    }
}

struct CheckResult: Sendable {
    var errors: Int32
    var warnings: Int32
    var dirty: Bool
    var hibernated: Bool
    var replayedTransactions: UInt64
    var files: UInt64
    var directories: UInt64
    var summary: String
}

/// Static engine calls that work on a device, not a mounted volume.
enum Engine {
    static var version: String { String(cString: bw_engine_version()) }

    static func probe(_ bridge: IOBridge) -> ProbeInfo {
        EngineQueue.assertOn()
        var p = bw_probe_info()
        _ = bw_probe(bridge.io, &p)
        let kind: ProbeInfo.Kind = switch Int32(p.kind) {
        case Int32(BW_KIND_NTFS): .ntfs
        case Int32(BW_KIND_BITLOCKER): .bitLocker
        case Int32(BW_KIND_REFS): .refs
        default: .notRecognized
        }
        return ProbeInfo(kind: kind, label: cString(p.label), serial: p.serial, dirty: p.dirty != 0,
                         majorVersion: Int(p.major_ver), minorVersion: Int(p.minor_ver),
                         clusterSize: p.cluster_size, totalBytes: p.total_bytes)
    }

    static func check(_ bridge: IOBridge, name: String, mode: CheckMode) throws(EngineError) -> CheckResult {
        EngineQueue.assertOn()
        var r = bw_fsck_result()
        let rc = bw_fsck(bridge.io, name, mode.raw, &r)
        try EngineError.check(rc)
        return CheckResult(errors: r.errors, warnings: r.warnings, dirty: r.dirty != 0, hibernated: r.hibernated != 0,
                           replayedTransactions: r.replayed_txns, files: r.files, directories: r.dirs,
                           summary: cString(r.summary))
    }
}

/// A mounted engine volume. Owns its IOBridge (the engine keeps the bw_io pointer).
/// Not thread-safe by itself: every call must be on EngineQueue.queue.
final class EngineVolume: @unchecked Sendable {
    let bridge: IOBridge
    private var handle: OpaquePointer?
    let mountResult: MountResult

    static func mount(_ bridge: IOBridge, name: String, readWrite: Bool, commitMilliseconds: Int, removable: Bool) throws(EngineError) -> EngineVolume {
        EngineQueue.assertOn()
        var opts = bw_mount_opts(want_rw: readWrite ? 1 : 0, commit_ms: Int32(commitMilliseconds), removable: removable ? 1 : 0)
        var info = bw_mount_info()
        var v: OpaquePointer?
        try EngineError.check(bw_mount(bridge.io, name, &opts, &v, &info))
        let r = MountResult(readOnly: info.read_only != 0, reasons: ReadOnlyReasons(rawValue: info.reasons),
                            journalEvent: info.journal_event, replayedTransactions: info.replayed_txns,
                            replayedBlocks: info.replayed_blocks, stale: info.stale != 0)
        return EngineVolume(bridge: bridge, handle: v!, result: r)
    }

    private init(bridge: IOBridge, handle: OpaquePointer, result: MountResult) {
        self.bridge = bridge
        self.handle = handle
        self.mountResult = result
    }

    deinit {
        // never leak a mount; the owner normally calls unmount() first
        if let h = handle { _ = bw_unmount(h) }
    }

    private func h() throws(EngineError) -> OpaquePointer {
        EngineQueue.assertOn()
        guard let handle else { throw EngineError(ENXIO) }
        return handle
    }

    var isMounted: Bool { handle != nil }

    func unmount() throws(EngineError) {
        let v = try h()
        handle = nil
        try EngineError.check(bw_unmount(v))
    }

    func sync() throws(EngineError) { try EngineError.check(bw_sync(try h())) }
    func commitDue() -> Bool { (try? h()).map { bw_commit_due($0) != 0 } ?? false }
    /// 0 healthy, else the errno that stopped the volume (fail-stop)
    func failure() -> Int32 { (try? h()).map { bw_failed($0) } ?? 0 }
    var isReadOnly: Bool { (try? h()).map { bw_read_only($0) != 0 } ?? true }

    func statistics() throws(EngineError) -> VolumeStats {
        var s = bw_statfs()
        try EngineError.check(bw_statfs_get(try h(), &s))
        return VolumeStats(clusterSize: s.cluster_size, totalClusters: s.total_clusters, freeClusters: s.free_clusters,
                           totalFiles: s.total_files, freeFiles: s.free_files, label: cString(s.label), serial: s.serial)
    }

    func attributes(_ ref: UInt64) throws(EngineError) -> ItemAttributes {
        var a = bw_attr()
        try EngineError.check(bw_getattr(try h(), ref, &a))
        return ItemAttributes(a)
    }

    func lookup(_ dir: UInt64, _ name: String) throws(EngineError) -> ItemAttributes {
        var a = bw_attr()
        try EngineError.check(bw_lookup(try h(), dir, name, &a))
        return ItemAttributes(a)
    }

    /// Up to `limit` entries after `cookie` (0 = start). Returns the entries and the cookie to
    /// continue from (0 = the end). No "." or ".."; system files hidden (bwntfs.h:115-119).
    func readDirectory(_ dir: UInt64, after cookie: UInt64, limit: Int) throws(EngineError) -> (entries: [DirectoryEntry], next: UInt64) {
        final class Box { var entries: [DirectoryEntry] = []; var limit = 0 }
        let box = Box()
        box.limit = limit
        var next: UInt64 = 0
        let v = try h()
        let rc = withExtendedLifetime(box) { () -> Int32 in
            bw_readdir(v, dir, cookie, Unmanaged.passUnretained(box).toOpaque(), { ctx, name, type, ref, nextCookie in
                let b = Unmanaged<Box>.fromOpaque(ctx!).takeUnretainedValue()
                if b.entries.count >= b.limit { return 1 }
                b.entries.append(DirectoryEntry(name: String(cString: name!), kind: ItemAttributes.Kind(rawValue: type) ?? .file,
                                                ref: ref, nextCookie: nextCookie))
                return 0
            }, &next)
        }
        try EngineError.check(rc)
        return (box.entries, next)
    }

    func create(_ dir: UInt64, _ name: String, kind: ItemAttributes.Kind, mode: UInt32) throws(EngineError) -> ItemAttributes {
        var a = bw_attr()
        try EngineError.check(bw_create(try h(), dir, name, kind.rawValue, mode, &a))
        return ItemAttributes(a)
    }

    func symlink(_ dir: UInt64, _ name: String, target: String) throws(EngineError) -> ItemAttributes {
        var a = bw_attr()
        try EngineError.check(bw_symlink(try h(), dir, name, target, &a))
        return ItemAttributes(a)
    }

    func readLink(_ ref: UInt64) throws(EngineError) -> String {
        var buf = [CChar](repeating: 0, count: Int(PATH_MAX) * 4)
        let v = try h()
        let rc = buf.withUnsafeMutableBufferPointer { bw_readlink(v, ref, $0.baseAddress, $0.count) }
        try EngineError.check(rc)
        return String(decoding: buf.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
    }

    func link(_ ref: UInt64, into dir: UInt64, name: String) throws(EngineError) {
        try EngineError.check(bw_link(try h(), ref, dir, name))
    }

    func remove(_ dir: UInt64, _ name: String, ref: UInt64) throws(EngineError) {
        try EngineError.check(bw_remove(try h(), dir, name, ref))
    }

    func rename(_ sdir: UInt64, _ sname: String, to ddir: UInt64, _ dname: String) throws(EngineError) {
        try EngineError.check(bw_rename(try h(), sdir, sname, ddir, dname))
    }

    /// EROFS for compressed or encrypted data opened for write, and for any write on a
    /// read-only volume (bwntfs.h:139).
    func openCheck(_ ref: UInt64, forWrite: Bool) throws(EngineError) {
        try EngineError.check(bw_open_check(try h(), ref, forWrite ? 1 : 0))
    }

    func read(_ ref: UInt64, at off: Int64, into buffer: UnsafeMutableRawBufferPointer) throws(EngineError) -> Int {
        Int(try EngineError.check64(bw_read(try h(), ref, off, Int64(buffer.count), buffer.baseAddress)))
    }

    func write(_ ref: UInt64, at off: Int64, from buffer: UnsafeRawBufferPointer) throws(EngineError) -> Int {
        Int(try EngineError.check64(bw_write(try h(), ref, off, Int64(buffer.count), buffer.baseAddress)))
    }

    func truncate(_ ref: UInt64, to size: Int64) throws(EngineError) {
        try EngineError.check(bw_truncate(try h(), ref, size))
    }

    func setTimes(_ ref: UInt64, modify: timespec?, access: timespec?, birth: timespec?, change: timespec?) throws(EngineError) {
        func bt(_ t: timespec?) -> bw_time? { t.map { bw_time(sec: Int64($0.tv_sec), nsec: Int64($0.tv_nsec)) } }
        var m = bt(modify), a = bt(access), b = bt(birth), c = bt(change)
        let v = try h()
        let rc = withOptionalPointer(&m) { mp in withOptionalPointer(&a) { ap in withOptionalPointer(&b) { bp in withOptionalPointer(&c) { cp in
            bw_set_times(v, ref, mp, ap, bp, cp) } } } }
        try EngineError.check(rc)
    }

    /// Sets the readonly and hidden bits (other BW_F_* bits are ignored by the engine).
    func setFlags(_ ref: UInt64, readOnly: Bool, hidden: Bool) throws(EngineError) {
        var f: UInt32 = 0
        if readOnly { f |= UInt32(BW_F_READONLY) }
        if hidden { f |= UInt32(BW_F_HIDDEN) }
        try EngineError.check(bw_set_flags(try h(), ref, f))
    }

    /// Device extents for [off, off+len) of the unnamed $DATA (bwntfs.h:148-155).
    func extents(_ ref: UInt64, at off: Int64, length: Int64, forWrite: Bool, max: Int = 64) throws(EngineError) -> [Extent] {
        var out = [bw_extent](repeating: bw_extent(), count: max)
        var n: Int32 = 0
        let v = try h()
        let rc = out.withUnsafeMutableBufferPointer { bw_extents(v, ref, off, length, forWrite ? 1 : 0, $0.baseAddress, Int32(max), &n) }
        try EngineError.check(rc)
        return out.prefix(Int(n)).map { Extent(logical: $0.logical, physical: $0.physical, length: $0.length, zeroFill: $0.zero_fill != 0) }
    }

    func completeWrite(_ ref: UInt64, end: Int64) throws(EngineError) {
        try EngineError.check(bw_complete_write(try h(), ref, end))
    }
}

private func withOptionalPointer<T, R>(_ value: inout T?, _ body: (UnsafePointer<T>?) -> R) -> R {
    if value == nil { return body(nil) }
    var v = value!
    return withUnsafePointer(to: &v) { body($0) }
}

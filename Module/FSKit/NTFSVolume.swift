// The NTFS volume. Every FSKit call hops onto EngineQueue.queue (the engine is single-threaded,
// bwntfs.h "Threading") with a synchronous, non-escaping closure, then replies.
//
// Protocols (FSVolume.h line numbers, MacOSX27.0 SDK; V1 protocols for the 15.4 target):
//   FSVolume.Operations 474 (+ CommonOperations 387, PathConfOperations 262)
//   FSVolume.OpenCloseOperations 1225, FSVolume.ReadWriteOperations 1319
//   FSVolumeKernelOffloadedIOOperations (FSVolumeExtent.h:130)
// Data moves through kernel-offloaded I/O (extents); files the engine flags BW_F_NO_KOIO
// (resident, compressed, encrypted, sparse) report inhibitKernelOffloadedIO (FSItem.h:231) and
// go through read/write here.
// UNVERIFIED until ADP: FSKit routes a file with inhibitKernelOffloadedIO to ReadWriteOperations
// on a volume that conforms to both protocols, and re-reads the attribute when a resident file
// grows non-resident.
//
// Extended attributes are out of scope for wave 1: the volume does not conform to
// XattrOperations, so FSKit answers ENOTSUP (named streams stay invisible, untouched).
import BWNTFSEngine
import FSKit
import Foundation

final class NTFSVolume: FSVolume, FSVolume.Operations, FSVolume.OpenCloseOperations,
    FSVolume.ReadWriteOperations, FSVolumeKernelOffloadedIOOperations, @unchecked Sendable {

    let resource: FSBlockDeviceResource
    let bridge: IOBridge
    let probe: ProbeInfo
    let load: LoadOptions
    let removable: Bool
    let policy: PolicyStore

    // Engine state: EngineQueue.queue only.
    private var engine: EngineVolume?
    private var commit: GroupCommit?
    private var items: [UInt64: NTFSItem] = [:]
    private var verifier: UInt64 = 1
    private var failureReported = false

    init(resource: FSBlockDeviceResource, bridge: IOBridge, probe: ProbeInfo, load: LoadOptions,
         removable: Bool, policy: PolicyStore) {
        self.resource = resource
        self.bridge = bridge
        self.probe = probe
        self.load = load
        self.removable = removable
        self.policy = policy
        let name = probe.label.isEmpty ? "Untitled" : probe.label
        // FSVolume.h:248 init(volumeID:volumeName:)
        super.init(volumeID: FSVolume.Identifier(uuid: probe.containerUUID), volumeName: FSFileName(string: name))
    }

    var isActiveOnQueue: Bool { engine?.isMounted ?? false }

    // MARK: engine helpers (queue only)

    private func mounted() throws -> EngineVolume {
        guard let engine, engine.isMounted else { throw posixError(ENXIO) }
        return engine
    }

    /// The volume accepts changes: mounted read-write and not failed. Fail-stop => EROFS.
    private func writable() throws -> EngineVolume {
        let e = try mounted()
        if e.failure() != 0 { reportFailure(e.failure()); throw posixError(EROFS) }
        guard !e.isReadOnly else { throw posixError(EROFS) }
        return e
    }

    /// After any engine error: a fail-stop turns the volume read-only; tell the log and the app.
    private func checkFailure() {
        if let e = engine, e.isMounted, e.failure() != 0 { reportFailure(e.failure()) }
    }

    private func reportFailure(_ code: Int32) {
        guard !failureReported else { return }
        failureReported = true
        commit?.stop()
        ModuleLog.fs.error("\(self.resource.bsdName, privacy: .public): write failed (errno \(code)); the volume is read-only until it is mounted again")
        var reasons = engine?.mountResult.reasons ?? []
        reasons.insert(.failed)
        policy.writeStatus(serial: probe.serial, label: probe.label, readOnly: true, reasons: reasons, failure: code)
    }

    private func item(_ ref: UInt64) -> NTFSItem {
        let key = ItemID.fs(ref) == .rootDirectory ? BW_ROOT_REF : ref
        if let i = items[key] { return i }
        let i = NTFSItem(ref: key)
        items[key] = i
        return i
    }

    private func ref(_ item: FSItem) throws -> UInt64 {
        guard let r = item.engineRef else { throw posixError(EINVAL) }
        return r
    }

    private func name(_ n: FSFileName) throws -> String {
        guard let s = n.string, !s.isEmpty, !s.contains("/") else { throw posixError(EINVAL) }
        return s
    }

    private func namespaceChanged() { verifier &+= 1; if verifier == 0 { verifier = 1 } }

    /// Runs `body` on the engine queue; engine errors become POSIX NSErrors and a fail-stop
    /// is reported.
    private func run<T>(_ body: () throws -> T) -> Result<T, any Error> {
        EngineQueue.run {
            do { return .success(try body()) } catch {
                self.checkFailure()
                return .failure(fsError(error))
            }
        }
    }

    /// Called by NTFSFileSystem.unloadResource on the queue.
    func shutdownOnQueue() -> (any Error)? {
        EngineQueue.assertOn()
        commit?.stop(); commit = nil
        guard let e = engine, e.isMounted else { engine = nil; return nil }
        defer { engine = nil; items.removeAll() }
        do { try e.unmount(); return nil } catch { return posixError(error.code) }
    }

    // MARK: PathConfOperations (FSVolume.h:262)

    var maximumLinkCount: Int { 1024 }          // NTFS: 1024 names per file record
    var maximumNameLength: Int { 255 }          // UTF-16 units per $FILE_NAME
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }

    // MARK: CommonOperations (FSVolume.h:387)

    /// FSVolume.h:135-219. Lookups are case-insensitive and names keep their case
    /// (engine dir_lookup, bw_api.c:356-363).
    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsPersistentObjectIDs = true
        c.supportsSymbolicLinks = true
        c.supportsHardLinks = true
        c.supportsJournal = true
        c.supportsActiveJournal = EngineQueue.run { !(engine?.isReadOnly ?? true) }
        c.supportsHiddenFiles = true
        c.supports64BitObjectIDs = true
        c.supports2TBFiles = true
        c.supportsFastStatFS = true
        c.doesNotSupportImmutableFiles = true
        c.caseFormat = .insensitiveCasePreserving
        return c
    }

    /// FSVolume.h:335 FSStatFSResult.
    /// UNVERIFIED until ADP: how the f_fstypename "ntfs" (Apple's own read-only ntfs uses the
    /// same name) shows in Finder and `mount` for an FSKit volume.
    var volumeStatistics: FSStatFSResult {
        let r = FSStatFSResult(fileSystemTypeName: "ntfs")
        let s: VolumeStats? = EngineQueue.run { try? engine?.statistics() }
        let bs = Int(s?.clusterSize ?? probe.clusterSize)
        r.blockSize = bs
        r.ioSize = max(bs, 1 << 20)
        if let s {
            let total = s.totalClusters, free = s.freeClusters
            r.totalBlocks = total
            r.freeBlocks = free
            r.availableBlocks = free
            r.usedBlocks = total - min(total, free)
            r.totalBytes = total &* UInt64(bs)
            r.freeBytes = free &* UInt64(bs)
            r.availableBytes = r.freeBytes
            r.usedBytes = r.usedBlocks &* UInt64(bs)
            r.totalFiles = s.totalFiles
            r.freeFiles = s.freeFiles
        } else {
            r.totalBytes = probe.totalBytes
            r.totalBlocks = bs > 0 ? probe.totalBytes / UInt64(bs) : 0
        }
        return r
    }

    /// FSVolume.h:425. Nothing to do: the engine mounted in activate.
    func mount(options: FSTaskOptions, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        reply(nil)
    }

    /// FSVolume.h:435: commit on unmount (the engine closes in deactivate).
    func unmount(replyHandler reply: @escaping @Sendable () -> Void) {
        _ = run { if let e = engine, e.isMounted, !e.isReadOnly { try e.sync() } }
        reply()
    }

    /// FSVolume.h:445: sync and eject commit the journal now.
    func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let r = run { if let e = engine, e.isMounted, !e.isReadOnly { try e.sync() } }
        if case .failure(let e) = r { reply(e) } else { reply(nil) }
    }

    /// FSVolume.h:458.
    func reclaimItem(_ item: FSItem, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        if let r = item.engineRef, r != BW_ROOT_REF {
            EngineQueue.run { _ = items.removeValue(forKey: r) }
        }
        reply(nil)
    }

    // MARK: Operations (FSVolume.h:474)

    /// FSVolume.h:487. Mounts the engine: journal replay first; policy from the app group
    /// (default read-only); dirty / hibernated / read-only device => read-only.
    func activate(options: FSTaskOptions, replyHandler reply: @escaping @Sendable (FSItem?, (any Error)?) -> Void) {
        let result: Result<FSItem, any Error> = run {
            if let e = engine, e.isMounted { return item(BW_ROOT_REF) }
            let drivePolicy = policy.policy(serial: probe.serial)
            let wantRW = drivePolicy == .readWrite && !load.readOnly && resource.isWritable
            let interval = CommitInterval.milliseconds(removable: removable)
            let e = try EngineVolume.mount(bridge, name: resource.bsdName, readWrite: wantRW,
                                           commitMilliseconds: interval, removable: removable)
            engine = e
            failureReported = false
            var reasons = e.mountResult.reasons
            if load.readOnly { reasons.insert(.policy) }
            policy.writeStatus(serial: probe.serial, label: probe.label, readOnly: e.isReadOnly, reasons: reasons, failure: 0)
            let mode = e.isReadOnly ? "read-only (\(reasons.explanations.joined(separator: "; ")))" : "read-write, commit every \(interval) ms"
            ModuleLog.fs.info("\(self.resource.bsdName, privacy: .public): mounted \(mode, privacy: .public); replayed \(e.mountResult.replayedTransactions) transactions")
            if !e.isReadOnly {
                let gc = GroupCommit(volume: e, intervalMilliseconds: interval) { [weak self] code in
                    self?.reportFailure(code)
                }
                commit = gc
                gc.start()
            }
            return item(BW_ROOT_REF)
        }
        switch result {
        case .success(let i): reply(i, nil)
        case .failure(let e): reply(nil, e)
        }
    }

    /// FSVolume.h:506: commit and close.
    func deactivate(options: FSDeactivateOptions = [], replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let err: (any Error)? = EngineQueue.run { shutdownOnQueue() }
        reply(err)
    }

    /// FSVolume.h:520.
    func lookupItem(named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        let r = run { () throws -> FSItem in
            let a = try mounted().lookup(try ref(directory), try self.name(name))
            return item(a.ref)
        }
        switch r {
        case .success(let i): reply(i, name, nil)
        case .failure(let e): reply(nil, nil, e)
        }
    }

    /// Creates a file or directory and applies the requested attributes (FSVolume.h:539).
    private func create(_ name: FSFileName, type: FSItem.ItemType, in directory: FSItem, attributes: FSItem.SetAttributesRequest) throws -> FSItem {
        let e = try writable()
        guard let kind = ItemID.engineKind(type), kind != .symlink else { throw posixError(ENOTSUP) }
        let mode = attributes.isValid(.mode) ? attributes.mode & 0o7777 : (kind == .directory ? 0o755 : 0o644)
        let a = try e.create(try ref(directory), try self.name(name), kind: kind, mode: mode)
        namespaceChanged()
        var consumed: FSItem.Attribute = attributes.isValid(.mode) ? [.mode, .type] : [.type]
        consumed.formUnion(try apply(attributes, to: a, skipping: [.mode, .type], engine: e))
        attributes.consumedAttributes = consumed
        return item(a.ref)
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        switch run({ try create(name, type: type, in: directory, attributes: newAttributes) }) {
        case .success(let i): reply(i, name, nil)
        case .failure(let e): reply(nil, nil, e)
        }
    }

    /// FSVolume.h:558. NTFS symlinks are reparse points (the ntfs-3g "native" symlink form).
    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        let r = run { () throws -> FSItem in
            let e = try writable()
            guard let target = contents.string, !target.isEmpty else { throw posixError(EINVAL) }
            let a = try e.symlink(try ref(directory), try self.name(name), target: target)
            namespaceChanged()
            newAttributes.consumedAttributes = [.type, .mode]
            return item(a.ref)
        }
        switch r {
        case .success(let i): reply(i, name, nil)
        case .failure(let e): reply(nil, nil, e)
        }
    }

    /// FSVolume.h:580.
    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        let r = run {
            try writable().link(try ref(item), into: try ref(directory), name: try self.name(name))
            namespaceChanged()
        }
        if case .failure(let e) = r { reply(nil, e) } else { reply(name, nil) }
    }

    /// FSVolume.h:622. Not atomic: libntfs-3g has no rename primitive (bwntfs.h bw_rename).
    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName, to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        let r = run {
            try writable().rename(try ref(sourceDirectory), try name(sourceName), to: try ref(destinationDirectory), try name(destinationName))
            namespaceChanged()
        }
        if case .failure(let e) = r { reply(nil, e) } else { reply(destinationName, nil) }
    }

    /// FSVolume.h:642.
    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let r = run {
            try writable().remove(try ref(directory), try self.name(name), ref: try ref(item))
            namespaceChanged()
        }
        if case .failure(let e) = r { reply(e) } else { reply(nil) }
    }

    /// FSVolume.h:658.
    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem, replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void) {
        switch run({ try mounted().attributes(try ref(item)).fsAttributes(wanted: desiredAttributes) }) {
        case .success(let a): reply(a, nil)
        case .failure(let e): reply(nil, e)
        }
    }

    /// Applies size, times, flags and mode; returns what it consumed (FSItem.h SetAttributesRequest).
    /// Mode: NTFS has no POSIX bits; removing every write bit sets FILE_ATTR_READONLY, adding one
    /// clears it. uid/gid are accepted and ignored (no owners on NTFS, like msdos/exfat).
    private func apply(_ req: FSItem.SetAttributesRequest, to current: ItemAttributes, skipping: FSItem.Attribute = [], engine e: EngineVolume) throws -> FSItem.Attribute {
        var used: FSItem.Attribute = []
        func wants(_ a: FSItem.Attribute) -> Bool { !skipping.contains(a) && req.isValid(a) }
        if wants(.size) {
            guard current.kind == .file else { throw posixError(current.kind == .directory ? EISDIR : EINVAL) }
            try e.truncate(current.ref, to: Int64(clamping: req.size))
            used.insert(.size)
        }
        let m = wants(.modifyTime) ? req.modifyTime : nil
        let at = wants(.accessTime) ? req.accessTime : nil
        let b = wants(.birthTime) ? req.birthTime : nil
        let c = wants(.changeTime) ? req.changeTime : nil
        if m != nil || at != nil || b != nil || c != nil {
            try e.setTimes(current.ref, modify: m, access: at, birth: b, change: c)
            if m != nil { used.insert(.modifyTime) }
            if at != nil { used.insert(.accessTime) }
            if b != nil { used.insert(.birthTime) }
            if c != nil { used.insert(.changeTime) }
        }
        if wants(.flags) || wants(.mode) {
            let hidden = wants(.flags) ? req.flags & UInt32(UF_HIDDEN) != 0 : current.isHidden
            let readOnly = wants(.mode) ? req.mode & 0o222 == 0 : current.isReadOnlyFlag
            if hidden != current.isHidden || readOnly != current.isReadOnlyFlag {
                try e.setFlags(current.ref, readOnly: readOnly, hidden: hidden)
            }
            if wants(.flags) { used.insert(.flags) }
            if wants(.mode) { used.insert(.mode) }
        }
        if wants(.uid) { used.insert(.uid) }
        if wants(.gid) { used.insert(.gid) }
        return used
    }

    /// FSVolume.h:684 (setattr; truncate is a size change).
    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem, replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void) {
        let r = run { () throws -> FSItem.Attributes in
            let e = try writable()
            let r = try ref(item)
            newAttributes.consumedAttributes = try apply(newAttributes, to: try e.attributes(r), engine: e)
            return try e.attributes(r).fsAttributes()
        }
        switch r {
        case .success(let a): reply(a, nil)
        case .failure(let e): reply(nil, e)
        }
    }

    /// FSVolume.h:711 (doc 690-700). Cookie scheme: DirCursor (Core/EngineIO.swift).
    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker, replyHandler reply: @escaping @Sendable (FSDirectoryVerifier, (any Error)?) -> Void) {
        let r = run { () throws -> UInt64 in
            let e = try mounted()
            let dir = try ref(directory)
            var start = DirCursor.decode(cookie.rawValue)
            if attributes != nil { start.dot = false; start.dotDot = false }
            if start.dot {
                guard packer.packEntry(name: FSFileName(string: "."), itemType: .directory, itemID: ItemID.fs(dir),
                                       nextCookie: FSDirectoryCookie(DirCursor.afterDot), attributes: nil) else { return self.verifier }
            }
            if start.dotDot {
                let parent = ItemID.fs(dir) == .rootDirectory ? ItemID.fs(dir) : ItemID.fs(try e.attributes(dir).parentRef)
                guard packer.packEntry(name: FSFileName(string: ".."), itemType: .directory, itemID: parent,
                                       nextCookie: FSDirectoryCookie(DirCursor.afterDotDot), attributes: nil) else { return self.verifier }
            }
            var engineCookie = start.engine
            while true {
                let page: (entries: [DirectoryEntry], next: UInt64)
                do { page = try e.readDirectory(dir, after: engineCookie, limit: 64) } catch let error as EngineError where error.code == EINVAL && engineCookie != 0 {
                    throw FSError(.invalidDirectoryCookie)
                }
                for entry in page.entries {
                    let attrs = try attributes.map { try e.attributes(entry.ref).fsAttributes(wanted: $0) }
                    // Engine cookies are ntfs_readdir positions + 1 (bw_api.c rd_fill), never 0; past the end
                    // they resume with nothing left.
                    let next = FSDirectoryCookie(DirCursor.encode(engine: entry.nextCookie))
                    guard packer.packEntry(name: FSFileName(string: entry.name), itemType: ItemID.fsType(entry.kind),
                                           itemID: ItemID.fs(entry.ref), nextCookie: next, attributes: attrs) else { return self.verifier }
                }
                if page.next == 0 || page.entries.isEmpty { return self.verifier }
                engineCookie = page.next
            }
        }
        switch r {
        case .success(let v): reply(FSDirectoryVerifier(v), nil)
        case .failure(let e): reply(FSDirectoryVerifier(0), e)
        }
    }

    /// FSVolume.h:725. Junctions and mount points read back as their target path.
    /// UNVERIFIED until ADP: how macOS clients follow a junction target that names a Windows drive.
    func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        switch run({ try mounted().readLink(try ref(item)) }) {
        case .success(let s): reply(FSFileName(string: s), nil)
        case .failure(let e): reply(nil, e)
        }
    }

    // MARK: OpenCloseOperations (FSVolume.h:1225)

    /// FSVolume.h:1241. Opening compressed or encrypted data for write => EROFS (bw_open_check).
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let r = run {
            let e = try mounted()
            let r = try ref(item)
            if modes.contains(.write) {
                _ = try writable()
                try e.openCheck(r, forWrite: true)
            }
        }
        if case .failure(let e) = r { reply(e) } else { reply(nil) }
    }

    /// FSVolume.h:1252.
    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        reply(nil)
    }

    // MARK: ReadWriteOperations (FSVolume.h:1319), for files with inhibitKernelOffloadedIO

    /// FSVolume.h:1332.
    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer, replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void) {
        let r = run { () throws -> Int in
            let e = try mounted()
            let r = try ref(item)
            return try buffer.withUnsafeMutableBytes { (raw: UnsafeMutableRawBufferPointer) throws -> Int in
                let n = min(length, raw.count)
                return try e.read(r, at: offset, into: UnsafeMutableRawBufferPointer(rebasing: raw[0..<n]))
            }
        }
        switch r {
        case .success(let n): reply(n, nil)
        case .failure(let e): reply(0, e)
        }
    }

    /// FSVolume.h ~1355: extends the file as needed; ENOSPC when the volume is full.
    func write(contents: Data, to item: FSItem, at offset: off_t, replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void) {
        let r = run { () throws -> Int in
            let e = try writable()
            let r = try ref(item)
            return try contents.withUnsafeBytes { (raw: UnsafeRawBufferPointer) throws -> Int in
                try e.write(r, at: offset, from: raw)
            }
        }
        switch r {
        case .success(let n): reply(n, nil)
        case .failure(let e): reply(0, e)
        }
    }

    // MARK: Kernel-offloaded I/O (FSVolumeExtent.h:130)

    /// FSVolumeExtent.h:152. Packs up to 64 extents per call; when the packer is full or the
    /// range is not covered yet FSKit asks again for the rest.
    /// UNVERIFIED until ADP: FSKit re-requests an uncovered tail; zeroFill extents on a
    /// non-sparse-capable volume; the kernel's UBC stays coherent with ReadWrite-served files.
    func blockmapFile(_ file: FSItem, offset: off_t, length: Int, flags: FSBlockmapFlags, operationID: FSOperationID, packer: FSExtentPacker, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let r = run {
            let forWrite = flags.contains(.write)
            let e = forWrite ? try writable() : try mounted()
            let extents = try e.extents(try ref(file), at: offset, length: Int64(length), forWrite: forWrite)
            for x in extents {
                let ok = packer.packExtent(resource: resource, type: x.zeroFill ? .zeroFill : .data,
                                           logicalOffset: x.logical, physicalOffset: x.physical, length: Int(x.length))
                if !ok { break }
            }
        }
        if case .failure(let e) = r { reply(e) } else { reply(nil) }
    }

    /// FSVolumeExtent.h:175. `status` is documented "nil if no error" but imports non-optional.
    /// UNVERIFIED until ADP: success arrives as an NSError with code 0.
    func completeIO(for file: FSItem, offset: off_t, length: Int, status: any Error, flags: FSCompleteIOFlags, operationID: FSOperationID, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let ok = (status as NSError).code == 0
        guard flags.contains(.write), ok else { reply(nil); return }
        let r = run { try writable().completeWrite(try ref(file), end: offset + Int64(length)) }
        if case .failure(let e) = r { reply(e) } else { reply(nil) }
    }

    /// FSVolumeExtent.h:208. The packer may stay empty; the first write asks blockmapFile.
    func createFile(name: FSFileName, in directory: FSItem, attributes: FSItem.SetAttributesRequest, packer: FSExtentPacker, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        switch run({ try create(name, type: .file, in: directory, attributes: attributes) }) {
        case .success(let i): reply(i, name, nil)
        case .failure(let e): reply(nil, nil, e)
        }
    }

    /// FSVolumeExtent.h:228. The packer may stay empty.
    func lookupItem(name: FSFileName, in directory: FSItem, packer: FSExtentPacker, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        lookupItem(named: name, inDirectory: directory, replyHandler: reply)
    }
}

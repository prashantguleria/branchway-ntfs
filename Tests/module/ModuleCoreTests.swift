// Module core tests: the Swift layer the FSKit glue calls (Module/Core), on image files.
// Engine calls run on EngineQueue like in the module.
import BWNTFSEngine
import Darwin
import FSKit
import XCTest

final class ModuleCoreTests: XCTestCase {
    private func mount(_ path: String, rw: Bool, commitMs: Int = 1000, alignment: Int = 4096) throws -> (FileBlockDevice, IOBridge, EngineVolume) {
        let dev = try FileBlockDevice(path: path, alignment: alignment)
        let bridge = IOBridge(device: dev)
        let v = try EngineQueue.run { () throws -> EngineVolume in
            try EngineVolume.mount(bridge, name: "test", readWrite: rw, commitMilliseconds: commitMs, removable: false)
        }
        return (dev, bridge, v)
    }

    private func unmount(_ v: EngineVolume) throws {
        try EngineQueue.run { () throws -> Void in try v.unmount() }
    }

    // MARK: bridge

    func testBridgeUnalignedIOMatchesFile() throws {
        let path = try Fixture.copy()
        for alignment in [512, 4096] {
            let dev = try FileBlockDevice(path: path, alignment: alignment)
            let bridge = IOBridge(device: dev)
            let fd = open(path, O_RDONLY); defer { close(fd) }
            for (off, n) in [(0, 512), (1, 7), (4095, 2), (511, 4097), (12345, 70000)] as [(Int64, Int64)] {
                var a = [UInt8](repeating: 0, count: Int(n)), b = a
                let got = a.withUnsafeMutableBytes { bridge.pread($0.baseAddress, n, off) }
                XCTAssertEqual(got, n)
                _ = b.withUnsafeMutableBytes { Darwin.pread(fd, $0.baseAddress, Int(n), off) }
                XCTAssertEqual(a, b, "off \(off) n \(n) align \(alignment)")
            }
            // read-modify-write keeps the neighbours
            let payload: [UInt8] = Array("branchway".utf8)
            let wrote = payload.withUnsafeBytes { bridge.pwrite($0.baseAddress, Int64(payload.count), 100_003) }
            XCTAssertEqual(wrote, Int64(payload.count))
            var back = [UInt8](repeating: 0, count: payload.count + 2)
            _ = back.withUnsafeMutableBytes { Darwin.pread(fd, $0.baseAddress, payload.count + 2, 100_002) }
            XCTAssertEqual(Array(back[1...payload.count]), payload)
            XCTAssertEqual(dev.unalignedRequests, 0)
        }
    }

    func testBridgeReadOnlyDeviceRefusesWrites() throws {
        let dev = try FileBlockDevice(path: try Fixture.copy(), writable: false)
        let bridge = IOBridge(device: dev)
        var b: UInt8 = 1
        XCTAssertEqual(bridge.pwrite(&b, 1, 0), -Int64(EROFS))
    }

    // MARK: probe

    func testProbeRecognizesNTFS() throws {
        let bridge = IOBridge(device: try FileBlockDevice(path: try Fixture.copy()))
        let p = EngineQueue.run { Engine.probe(bridge) }
        XCTAssertEqual(p.kind, .ntfs)
        XCTAssertEqual(p.label, "MODTEST")
        XCTAssertFalse(p.dirty)
        XCTAssertNotEqual(p.serial, 0)
        XCTAssertEqual(p.containerUUID, p.containerUUID)
        XCTAssertEqual(p.containerUUID.uuidString.count, 36)
    }

    private func probe(boot: [UInt8], at: Int) throws -> ProbeInfo.Kind {
        let path = Fixture.dir + "/work/probe-\(UUID().uuidString).img"
        defer { try? FileManager.default.removeItem(atPath: path) }
        var data = Data(count: 1 << 20)
        data.replaceSubrange(at..<at + boot.count, with: boot)
        try data.write(to: URL(fileURLWithPath: path))
        let bridge = IOBridge(device: try FileBlockDevice(path: path))
        return EngineQueue.run { Engine.probe(bridge).kind }
    }

    func testProbeDeclinesOthers() throws {
        XCTAssertEqual(try probe(boot: [], at: 0), .notRecognized)
        XCTAssertEqual(try probe(boot: Array("-FVE-FS-".utf8), at: 3), .bitLocker)
        XCTAssertEqual(try probe(boot: Array("ReFS".utf8) + [0, 0, 0, 0], at: 3), .refs)
        XCTAssertEqual(try probe(boot: Array("EXFAT   ".utf8), at: 3), .notRecognized)
    }

    // MARK: mount policy

    func testReadOnlyPolicyMount() throws {
        let (_, _, v) = try mount(try Fixture.copy(), rw: false)
        XCTAssertTrue(EngineQueue.run { v.isReadOnly })
        XCTAssertTrue(v.mountResult.reasons.contains(.policy))
        let err = EngineQueue.run { () -> Int32 in
            do { _ = try v.create(BW_ROOT_REF, "x", kind: .file, mode: 0o644); return 0 } catch { return EngineError.errno(of: error) }
        }
        XCTAssertEqual(err, EROFS)
        let open = EngineQueue.run { () -> Int32 in
            do { let a = try v.lookup(BW_ROOT_REF, "big.bin"); try v.openCheck(a.ref, forWrite: true); return 0 } catch { return EngineError.errno(of: error) }
        }
        XCTAssertEqual(open, EROFS)
        try unmount(v)
    }

    func testDirtyAndHibernatedMountReadOnly() throws {
        for (img, reason) in [("dirty.img", ReadOnlyReasons.dirty), ("hiber.img", .hibernated)] {
            let (_, _, v) = try mount(try Fixture.copy(img), rw: true)
            XCTAssertTrue(EngineQueue.run { v.isReadOnly }, img)
            XCTAssertTrue(v.mountResult.reasons.contains(reason), img)
            XCTAssertFalse(reason.explanations.isEmpty)
            try unmount(v)
        }
    }

    // MARK: data path

    func testCreateWriteReadRename() throws {
        let path = try Fixture.copy()
        var (_, _, v) = try mount(path, rw: true)
        XCTAssertFalse(EngineQueue.run { v.isReadOnly })
        let payload = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 7) })
        try EngineQueue.run { () throws -> Void in
            let d = try v.create(BW_ROOT_REF, "Docs", kind: .directory, mode: 0o755)
            let f = try v.create(d.ref, "Report.TXT", kind: .file, mode: 0o644)
            let n = try payload.withUnsafeBytes { try v.write(f.ref, at: 0, from: $0) }
            XCTAssertEqual(n, payload.count)
            try v.rename(d.ref, "Report.TXT", to: BW_ROOT_REF, "moved.txt")
            // case-insensitive lookup, case kept
            let m = try v.lookup(BW_ROOT_REF, "MOVED.TXT")
            XCTAssertEqual(m.size, UInt64(payload.count))
            try v.truncate(m.ref, to: 1000)
            XCTAssertEqual(try v.attributes(m.ref).size, 1000)
            try v.sync()
        }
        try unmount(v)
        (_, _, v) = try mount(path, rw: false)
        try EngineQueue.run { () throws -> Void in
            let m = try v.lookup(BW_ROOT_REF, "moved.txt")
            var back = Data(count: 2000)
            let n = try back.withUnsafeMutableBytes { try v.read(m.ref, at: 0, into: $0) }
            XCTAssertEqual(n, 1000)
            XCTAssertEqual(back.prefix(1000), payload.prefix(1000))
            XCTAssertThrowsError(try v.lookup(try v.lookup(BW_ROOT_REF, "Docs").ref, "Report.TXT"))
        }
        try unmount(v)
    }

    func testReaddirPagingSeesEveryEntryOnce() throws {
        let (_, _, v) = try mount(try Fixture.copy(), rw: true)
        let names = Set((0..<150).map { "f\($0).dat" })
        try EngineQueue.run { () throws -> Void in
            let d = try v.create(BW_ROOT_REF, "many", kind: .directory, mode: 0o755)
            for n in names { _ = try v.create(d.ref, n, kind: .file, mode: 0o644) }
            // page through the way NTFSVolume.enumerateDirectory does, via FSKit cookies
            var seen: [String] = []
            var fsCookie: UInt64 = DirCursor.afterDotDot
            while true {
                let start = DirCursor.decode(fsCookie)
                let page = try v.readDirectory(d.ref, after: start.engine, limit: 7)
                guard !page.entries.isEmpty else { break }
                seen += page.entries.map(\.name)
                fsCookie = DirCursor.encode(engine: page.entries.last!.nextCookie)
            }
            XCTAssertEqual(seen.count, names.count)
            XCTAssertEqual(Set(seen), names)
            let root = try v.readDirectory(BW_ROOT_REF, after: 0, limit: 1000).entries.map(\.name)
            XCTAssertTrue(root.contains("hello.txt"))
            XCTAssertFalse(root.contains("$MFT"))
            XCTAssertFalse(root.contains("$BwJournal"))
        }
        try unmount(v)
    }

    func testDirCursorRoundTrip() {
        XCTAssertEqual(DirCursor.decode(0), .init(dot: true, dotDot: true, engine: 0))
        XCTAssertEqual(DirCursor.decode(1), .init(dot: false, dotDot: true, engine: 0))
        XCTAssertEqual(DirCursor.decode(2), .init(dot: false, dotDot: false, engine: 0))
        XCTAssertEqual(DirCursor.decode(DirCursor.encode(engine: 42)).engine, 42)
    }

    // MARK: kernel-offloaded I/O

    func testExtentsMapNonResidentDataAndRefuseResident() throws {
        let path = try Fixture.copy()
        let (dev, _, v) = try mount(path, rw: true)
        try EngineQueue.run { () throws -> Void in
            let big = try v.lookup(BW_ROOT_REF, "big.bin")
            XCTAssertFalse(big.inhibitsKernelIO)
            let ext = try v.extents(big.ref, at: 0, length: 1 << 20, forWrite: false)
            XCTAssertFalse(ext.isEmpty)
            XCTAssertEqual(ext.filter { !$0.zeroFill }.reduce(0) { $0 + $1.length }, 1 << 20)
            // the device bytes at the mapped extents are the file's bytes
            var got = Data()
            for x in ext where !x.zeroFill {
                var buf = [UInt8](repeating: 0, count: Int(x.length))
                _ = buf.withUnsafeMutableBytes { Darwin.pread(dev.fd, $0.baseAddress, Int(x.length), x.physical) }
                got.append(contentsOf: buf)
            }
            XCTAssertEqual(Array(got), (0..<(1 << 20)).map { UInt8($0 % 251) })

            let small = try v.lookup(BW_ROOT_REF, "hello.txt")
            XCTAssertTrue(small.inhibitsKernelIO)
            XCTAssertTrue(small.fsAttributes().inhibitKernelOffloadedIO)
            XCTAssertThrowsError(try v.extents(small.ref, at: 0, length: 4096, forWrite: false)) {
                XCTAssertEqual(($0 as? EngineError)?.code, ENOTSUP)
            }

            // a kernel write: allocate, write through the extents, complete
            let f = try v.create(BW_ROOT_REF, "kio.bin", kind: .file, mode: 0o644)
            try v.truncate(f.ref, to: 0)
            let w = try v.extents(f.ref, at: 0, length: 65536, forWrite: true)
            XCTAssertEqual(w.reduce(0) { $0 + $1.length }, 65536)
            for x in w {
                let buf = [UInt8](repeating: 0xAB, count: Int(x.length))
                _ = buf.withUnsafeBytes { Darwin.pwrite(dev.fd, $0.baseAddress, Int(x.length), x.physical) }
            }
            try v.completeWrite(f.ref, end: 65536)
            XCTAssertEqual(try v.attributes(f.ref).size, 65536)
            var back = [UInt8](repeating: 0, count: 65536)
            _ = try back.withUnsafeMutableBytes { try v.read(f.ref, at: 0, into: $0) }
            XCTAssertTrue(back.allSatisfy { $0 == 0xAB })
        }
        try unmount(v)
    }

    // MARK: group commit and fail-stop

    func testGroupCommitCommitsWhenDue() throws {
        let (dev, _, v) = try mount(try Fixture.copy(), rw: true, commitMs: 300)
        let failures = LockedBox<[Int32]>([])
        let gc = GroupCommit(volume: v, intervalMilliseconds: 300) { code in failures.mutate { $0.append(code) } }
        try EngineQueue.run { () throws -> Void in
            _ = try v.create(BW_ROOT_REF, "gc.txt", kind: .file, mode: 0o644)
        }
        usleep(400_000)
        let before = dev.barriers
        EngineQueue.run {
            XCTAssertTrue(v.commitDue())
            gc.tick()
            XCTAssertFalse(v.commitDue())
        }
        XCTAssertGreaterThan(dev.barriers, before)
        XCTAssertTrue(failures.value.isEmpty)
        try unmount(v)
    }

    func testFailStopTurnsReadOnlyAndReports() throws {
        let (dev, _, v) = try mount(try Fixture.copy(), rw: true, commitMs: 0)
        let reported = LockedBox<Int32>(0)
        let gc = GroupCommit(volume: v, intervalMilliseconds: 1000) { code in reported.mutate { $0 = code } }
        dev.failWrites = true
        EngineQueue.run {
            _ = try? v.create(BW_ROOT_REF, "doomed.txt", kind: .file, mode: 0o644)
            _ = try? v.sync()
            XCTAssertNotEqual(v.failure(), 0)
            XCTAssertTrue(v.isReadOnly)
            gc.tick()
        }
        XCTAssertNotEqual(reported.value, 0)
        dev.failWrites = false
        let err = EngineQueue.run { () -> Int32 in
            do { _ = try v.create(BW_ROOT_REF, "after.txt", kind: .file, mode: 0o644); return 0 } catch { return EngineError.errno(of: error) }
        }
        // The engine answers EIO after a fail-stop; NTFSVolume.writable() turns every mutation
        // into EROFS before it reaches the engine (Module/FSKit/NTFSVolume.swift).
        XCTAssertTrue(err == EIO || err == EROFS, "errno \(err)")
        EngineQueue.run { _ = try? v.unmount() }
    }

    // MARK: check

    func testCheckModes() throws {
        let bridge = IOBridge(device: try FileBlockDevice(path: try Fixture.copy()))
        for mode in [CheckMode.quick, .check, .repair] {
            let r = try EngineQueue.run { () throws -> CheckResult in try Engine.check(bridge, name: "t", mode: mode) }
            XCTAssertEqual(r.errors, 0, "\(mode): \(r.summary)")
            XCTAssertFalse(r.dirty)
        }
        let dirty = IOBridge(device: try FileBlockDevice(path: try Fixture.copy("dirty.img")))
        let r = try EngineQueue.run { () throws -> CheckResult in try Engine.check(dirty, name: "t", mode: .quick) }
        XCTAssertTrue(r.dirty)
    }

    // MARK: options, policy, mapping

    func testLoadAndCheckOptions() {
        XCTAssertTrue(LoadOptions(["--rdonly"]).readOnly)
        XCTAssertTrue(LoadOptions(["-f", "--rdonly"]).force)
        XCTAssertFalse(LoadOptions([]).readOnly)
        XCTAssertEqual(CheckOptions(["-y", "-q"]).mode, .quick)
        XCTAssertEqual(CheckOptions(["-y"]).mode, .repair)
        XCTAssertEqual(CheckOptions(["-n"]).mode, .check)
        XCTAssertEqual(CommitInterval.milliseconds(removable: true), 5000)
        XCTAssertEqual(CommitInterval.milliseconds(removable: false), 1000)
    }

    func testPolicyStoreDefaultsReadOnly() throws {
        let suite = "bwntfs.tests.\(UUID().uuidString)"
        let d = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { d.removePersistentDomain(forName: suite) }
        let s = PolicyStore(defaults: d)
        XCTAssertEqual(s.policy(serial: 0xABCD), .readOnly)
        s.setPolicy(.readWrite, serial: 0xABCD)
        XCTAssertEqual(s.policy(serial: 0xABCD), .readWrite)
        XCTAssertEqual(s.policy(serial: 0x1234), .readOnly)
        d.set("readWrite", forKey: PolicyStore.defaultKey)
        XCTAssertEqual(s.policy(serial: 0x1234), .readWrite)
        XCTAssertEqual(PolicyStore(defaults: nil).policy(serial: 1), .readOnly)
        XCTAssertEqual(PolicyStore.policyKey(serial: 0xABCD), "ntfs.policy.000000000000ABCD")
        s.writeStatus(serial: 0xABCD, label: "X", readOnly: true, reasons: [.dirty], failure: 0)
        let st = try XCTUnwrap(d.dictionary(forKey: PolicyStore.statusKey(serial: 0xABCD)))
        XCTAssertEqual(st["readOnly"] as? Bool, true)
        XCTAssertNil(PolicyStore.appGroup(bundle: Bundle(for: Self.self)).defaults)
    }

    func testAttributeMapping() throws {
        let (_, _, v) = try mount(try Fixture.copy(), rw: false)
        try EngineQueue.run { () throws -> Void in
            let root = try v.attributes(BW_ROOT_REF)
            XCTAssertEqual(ItemID.fs(root.ref), .rootDirectory)
            let ra = root.fsAttributes()
            XCTAssertEqual(ra.type, .directory)
            XCTAssertEqual(ra.parentID, .parentOfRoot)
            XCTAssertEqual(ra.mode & UInt32(S_IFMT), UInt32(S_IFDIR))
            let f = try v.lookup(BW_ROOT_REF, "big.bin")
            let fa = f.fsAttributes()
            XCTAssertEqual(fa.type, .file)
            XCTAssertEqual(fa.mode & UInt32(S_IFMT), UInt32(S_IFREG))
            XCTAssertEqual(fa.size, 1 << 20)
            XCTAssertEqual(fa.uid, Ownership.unknownID)
            XCTAssertEqual(fa.parentID, .rootDirectory)
            XCTAssertEqual(ItemID.engine(fa.fileID), f.ref)
            XCTAssertEqual(ItemID.engine(.rootDirectory), BW_ROOT_REF)
            XCTAssertFalse(fa.inhibitKernelOffloadedIO)
        }
        try unmount(v)
    }
}

final class LockedBox<T>: @unchecked Sendable {
    private var v: T
    private let lock = NSLock()
    init(_ v: T) { self.v = v }
    var value: T { lock.withLock { v } }
    func mutate(_ f: (inout T) -> Void) { lock.withLock { f(&v) } }
}

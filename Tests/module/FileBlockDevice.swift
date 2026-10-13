// A BlockDevice over an image file, for the tests. It enforces the alignment FSKit's raw I/O
// requires (FSResource.h:179), so an unaligned engine request fails loudly here.
import Darwin
import Foundation

final class FileBlockDevice: BlockDevice, @unchecked Sendable {
    let fd: Int32
    let byteCount: UInt64
    let alignment: Int
    let logicalBlockSize: Int
    let isWritable: Bool
    var failWrites = false
    private(set) var barriers = 0
    private(set) var unalignedRequests = 0

    init(path: String, alignment: Int = 4096, writable: Bool = true) throws {
        fd = open(path, writable ? O_RDWR : O_RDONLY)
        guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        var st = stat()
        fstat(fd, &st)
        byteCount = UInt64(st.st_size)
        self.alignment = alignment
        logicalBlockSize = 512
        isWritable = writable
    }

    deinit { close(fd) }

    private func aligned(_ count: Int, _ offset: Int64) -> Bool {
        count % alignment == 0 && offset % Int64(alignment) == 0
    }

    func readRaw(into buffer: UnsafeMutableRawBufferPointer, at offset: Int64) throws -> Int {
        guard aligned(buffer.count, offset) else { unalignedRequests += 1; throw POSIXError(.EINVAL) }
        let r = Darwin.pread(fd, buffer.baseAddress, buffer.count, offset)
        if r < 0 { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        return r
    }

    func writeRaw(from buffer: UnsafeRawBufferPointer, at offset: Int64) throws -> Int {
        guard aligned(buffer.count, offset) else { unalignedRequests += 1; throw POSIXError(.EINVAL) }
        if failWrites { throw POSIXError(.EIO) }
        let r = Darwin.pwrite(fd, buffer.baseAddress, buffer.count, offset)
        if r < 0 { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        return r
    }

    func barrier() throws {
        barriers += 1
        if failWrites { throw POSIXError(.EIO) }
        if fsync(fd) != 0 { throw POSIXError(.EIO) }
    }
}

/// Fresh copies of the fixtures from Scripts/module_test_fixture.sh.
enum Fixture {
    static var dir: String {
        ProcessInfo.processInfo.environment["BWNTFS_TEST_DIR"] ?? "/Volumes/Media/spikes/ntfs/s6work/ntfs2/module"
    }

    static func copy(_ name: String = "fixture.img", file: String = #function) throws -> String {
        let src = dir + "/" + name
        guard FileManager.default.fileExists(atPath: src) else {
            throw NSError(domain: "Fixture", code: 1, userInfo: [NSLocalizedDescriptionKey: "missing \(src): run Scripts/module_test_fixture.sh"])
        }
        let work = dir + "/work"
        try FileManager.default.createDirectory(atPath: work, withIntermediateDirectories: true)
        let tag = file.replacingOccurrences(of: "()", with: "")
        let dst = "\(work)/\(tag)-\(name)"
        try? FileManager.default.removeItem(atPath: dst)
        try FileManager.default.copyItem(atPath: src, toPath: dst)
        return dst
    }
}

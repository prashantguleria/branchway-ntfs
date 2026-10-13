// The engine's device backend: one serial queue for every engine call, and a bw_io built on a
// BlockDevice (FSBlockDeviceResource in the module, an image file in the tests).
//
// Engine/include/bwntfs.h:6-8: the engine never opens a path; every byte goes through bw_io.
// Engine/include/bwntfs.h:7-8: libntfs-3g is single-threaded, so the caller serializes every call
// (all volumes included) on one queue. EngineQueue.queue is that queue.
import BWNTFSEngine
import Darwin
import Foundation

/// The one serial queue every engine call runs on (bwntfs.h:7-8).
enum EngineQueue {
    static let queue = DispatchQueue(label: "com.evenpine.branchwayntfs.engine", qos: .userInitiated)

    static func assertOn() {
        dispatchPrecondition(condition: .onQueue(queue))
    }
}

/// An engine error: a positive POSIX errno (the engine returns -errno, bwntfs.h:10).
struct EngineError: Error, Equatable, CustomStringConvertible {
    let code: Int32
    init(_ code: Int32) { self.code = code }
    var description: String { "errno \(code) (\(String(cString: strerror(code))))" }

    /// Throws for a negative engine return; passes a non-negative one through.
    @discardableResult
    static func check(_ rc: Int32) throws(EngineError) -> Int32 {
        if rc < 0 { throw EngineError(-rc) }
        return rc
    }

    @discardableResult
    static func check64(_ rc: Int64) throws(EngineError) -> Int64 {
        if rc < 0 { throw EngineError(Int32(truncatingIfNeeded: -rc)) }
        return rc
    }

    /// The errno behind any error: POSIX-domain NSErrors keep their code, everything else is EIO.
    static func errno(of error: any Error) -> Int32 {
        if let e = error as? EngineError { return e.code }
        let ns = error as NSError
        if ns.domain == NSPOSIXErrorDomain, ns.code > 0 { return Int32(ns.code) }
        return EIO
    }
}

/// A block device as the engine sees it. Raw reads and writes must be aligned to
/// `alignment` in offset and length (FSBlockDeviceResource's direct I/O rule, FSResource.h:151);
/// they may transfer less than asked.
protocol BlockDevice: AnyObject {
    var byteCount: UInt64 { get }
    /// offset and length alignment for raw I/O
    var alignment: Int { get }
    /// the logical block size reported to the engine (512 or 4096)
    var logicalBlockSize: Int { get }
    var isWritable: Bool { get }
    func readRaw(into buffer: UnsafeMutableRawBufferPointer, at offset: Int64) throws -> Int
    func writeRaw(from buffer: UnsafeRawBufferPointer, at offset: Int64) throws -> Int
    /// every write returned so far is on stable media when this returns
    func barrier() throws
}

/// Builds the engine's bw_io over a BlockDevice. Unaligned engine I/O goes through a bounce
/// buffer (read-modify-write for writes). The bw_io lives at a fixed heap address because the
/// engine keeps the pointer for the life of the mount (Engine/nh_dev.c:290, `b->io = o->io`).
///
/// Only used on EngineQueue (the engine calls back synchronously from its own calls).
final class IOBridge: @unchecked Sendable {
    let device: any BlockDevice
    let io: UnsafeMutablePointer<bw_io>
    /// the last device error (for the fail-stop report)
    private(set) var lastError: Int32 = 0

    init(device: any BlockDevice) {
        self.device = device
        io = .allocate(capacity: 1)
        io.initialize(to: bw_io(
            ctx: nil,
            pread: { ctx, buf, n, off in IOBridge.from(ctx).pread(buf, n, off) },
            pwrite: { ctx, buf, n, off in IOBridge.from(ctx).pwrite(buf, n, off) },
            barrier: { ctx in IOBridge.from(ctx).barrierCall() },
            size: device.byteCount,
            block_size: UInt32(device.logicalBlockSize),
            writable: device.isWritable ? 1 : 0))
        io.pointee.ctx = Unmanaged.passUnretained(self).toOpaque()
    }

    deinit {
        io.deinitialize(count: 1)
        io.deallocate()
    }

    private static func from(_ ctx: UnsafeMutableRawPointer?) -> IOBridge {
        Unmanaged<IOBridge>.fromOpaque(ctx!).takeUnretainedValue()
    }

    private func fail(_ error: any Error) -> Int64 {
        lastError = EngineError.errno(of: error)
        return -Int64(lastError)
    }

    /// Reads [off, off+n) exactly, looping over partial transfers. Short only at the device end.
    private func rawLoop(read buffer: UnsafeMutableRawBufferPointer, at off: Int64) throws -> Int {
        var done = 0
        while done < buffer.count {
            let r = try device.readRaw(into: UnsafeMutableRawBufferPointer(rebasing: buffer[done...]), at: off + Int64(done))
            if r <= 0 { break }
            done += r
        }
        return done
    }

    private func rawLoop(write buffer: UnsafeRawBufferPointer, at off: Int64) throws -> Int {
        var done = 0
        while done < buffer.count {
            let r = try device.writeRaw(from: UnsafeRawBufferPointer(rebasing: buffer[done...]), at: off + Int64(done))
            if r <= 0 { throw EngineError(EIO) }
            done += r
        }
        return done
    }

    /// The aligned span that covers [off, off+n), clamped to the device end.
    private func span(_ off: Int64, _ n: Int64) -> (start: Int64, end: Int64) {
        let a = Int64(device.alignment)
        let start = off - off % a
        var end = (off + n + a - 1) / a * a
        end = min(end, Int64(device.byteCount))
        return (start, end)
    }

    func pread(_ buf: UnsafeMutableRawPointer?, _ n: Int64, _ off: Int64) -> Int64 {
        guard let buf, n > 0 else { return 0 }
        let a = Int64(device.alignment)
        do {
            if off % a == 0, n % a == 0 {
                return Int64(try rawLoop(read: UnsafeMutableRawBufferPointer(start: buf, count: Int(n)), at: off))
            }
            let (start, end) = span(off, n)
            if end <= off { return 0 }
            let bounce = UnsafeMutableRawBufferPointer.allocate(byteCount: Int(end - start), alignment: 4096)
            defer { bounce.deallocate() }
            let got = Int64(try rawLoop(read: bounce, at: start))
            let avail = max(0, min(n, got - (off - start)))
            if avail > 0 { buf.copyMemory(from: bounce.baseAddress! + Int(off - start), byteCount: Int(avail)) }
            return avail
        } catch {
            return fail(error)
        }
    }

    func pwrite(_ buf: UnsafeRawPointer?, _ n: Int64, _ off: Int64) -> Int64 {
        guard let buf, n > 0 else { return 0 }
        guard device.isWritable else { lastError = EROFS; return -Int64(EROFS) }
        let a = Int64(device.alignment)
        do {
            if off % a == 0, n % a == 0 {
                return Int64(try rawLoop(write: UnsafeRawBufferPointer(start: buf, count: Int(n)), at: off))
            }
            // read-modify-write of the covering aligned span
            let (start, end) = span(off, n)
            guard end >= off + n else { lastError = EIO; return -Int64(EIO) }
            let bounce = UnsafeMutableRawBufferPointer.allocate(byteCount: Int(end - start), alignment: 4096)
            defer { bounce.deallocate() }
            let got = try rawLoop(read: bounce, at: start)
            guard got == bounce.count else { lastError = EIO; return -Int64(EIO) }
            (bounce.baseAddress! + Int(off - start)).copyMemory(from: buf, byteCount: Int(n))
            _ = try rawLoop(write: UnsafeRawBufferPointer(bounce), at: start)
            return n
        } catch {
            return fail(error)
        }
    }

    func barrierCall() -> Int32 {
        do {
            try device.barrier()
            return 0
        } catch {
            return Int32(fail(error))
        }
    }
}

extension EngineQueue {
    /// Runs `body` on the engine queue and waits. FSKit calls the module on its own threads,
    /// never on this queue, so the wait cannot deadlock; the precondition proves it.
    static func run<T>(_ body: () throws -> T) rethrows -> T {
        dispatchPrecondition(condition: .notOnQueue(queue))
        return try queue.sync(execute: body)
    }
}

/// Directory cookies as FSKit sees them (FSVolume.h:711 "your implementation defines the
/// semantics of the cookie values"). When FSKit asks for no attributes, "." and ".." come first
/// (FSVolume.h:700 tip): cookie 0 = start, 1 = after ".", 2 = after ".." (engine start);
/// any other cookie is an engine cookie + 2 (engine cookies are nonzero positions; 0 = end).
enum DirCursor {
    struct Start: Equatable { var dot: Bool; var dotDot: Bool; var engine: UInt64 }

    static func decode(_ cookie: UInt64) -> Start {
        switch cookie {
        case 0: Start(dot: true, dotDot: true, engine: 0)
        case 1: Start(dot: false, dotDot: true, engine: 0)
        case 2: Start(dot: false, dotDot: false, engine: 0)
        default: Start(dot: false, dotDot: false, engine: cookie - 2)
        }
    }

    static func encode(engine: UInt64) -> UInt64 { engine + 2 }
    static let afterDot: UInt64 = 1
    static let afterDotDot: UInt64 = 2
}

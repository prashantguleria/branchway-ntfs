// BlockDevice over FSBlockDeviceResource (FSResource.h:132-200).
//
// Raw read/write (the synchronous throwing variants) bypass the buffer cache and must be
// aligned to physicalBlockSize (FSResource.h:179); the engine's unaligned I/O is bounced by
// IOBridge. The engine never uses metadataRead/Write, so direct and metadata I/O never mix on a
// range (FSResource.h:149).
//
// UNVERIFIED until ADP: metadataFlush() (FSResource.h, "metadataFlush") is documented as a flush
// of the resource's cached metadata writes; that it also issues a device cache flush
// (DKIOCSYNCHRONIZE / F_FULLFSYNC equivalent) for writes made with write(from:startingAt:length:)
// is not documented. The journal's ordering barrier depends on it.
import FSKit

final class FSBlockDevice: BlockDevice {
    let resource: FSBlockDeviceResource

    init(_ resource: FSBlockDeviceResource) { self.resource = resource }

    var byteCount: UInt64 { resource.blockSize &* resource.blockCount }
    var alignment: Int { Int(max(resource.physicalBlockSize, resource.blockSize, 512)) }
    var logicalBlockSize: Int { Int(resource.blockSize) }
    var isWritable: Bool { resource.isWritable }

    func readRaw(into buffer: UnsafeMutableRawBufferPointer, at offset: Int64) throws -> Int {
        try resource.read(into: buffer, startingAt: offset, length: buffer.count)
    }

    func writeRaw(from buffer: UnsafeRawBufferPointer, at offset: Int64) throws -> Int {
        try resource.write(from: buffer, startingAt: offset, length: buffer.count)
    }

    func barrier() throws {
        try resource.metadataFlush()
    }
}

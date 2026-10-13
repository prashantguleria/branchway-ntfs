// Engine items <-> FSKit items: identifiers and attributes.
//
// FSItem.Identifier (FSItem.h:324 in the synthesized Swift interface; FSItemID in FSItem.h):
// 0 invalid, 1 parent of root, 2 root. NTFS's root is MFT record 5 (BW_ROOT_REF); every other
// item uses its full MFT reference (record | sequence << 48). Real references always carry a
// nonzero sequence number, so they never collide with 0, 1 or 2.
import BWNTFSEngine
import Darwin
import FSKit

enum ItemID {
    static func fs(_ ref: UInt64) -> FSItem.Identifier {
        if ref == BW_ROOT_REF || ref & 0x0000_FFFF_FFFF_FFFF == BW_ROOT_REF { return .rootDirectory }
        if ref == 0 { return .invalid }
        // Imported C enums accept any raw value.
        return FSItem.Identifier(rawValue: ref) ?? .invalid
    }

    static func engine(_ id: FSItem.Identifier) -> UInt64 {
        id == .rootDirectory ? BW_ROOT_REF : id.rawValue
    }

    static func fsType(_ kind: ItemAttributes.Kind) -> FSItem.ItemType {
        switch kind {
        case .file: .file
        case .directory: .directory
        case .symlink: .symlink
        }
    }

    static func engineKind(_ type: FSItem.ItemType) -> ItemAttributes.Kind? {
        switch type {
        case .file: .file
        case .directory: .directory
        case .symlink: .symlink
        default: nil
        }
    }
}

/// NTFS has no POSIX owners. Like msdos and exfat, every item reports the "unknown" user and
/// group (99), which macOS shows as the person viewing the volume.
/// UNVERIFIED until ADP: FSKit presents uid/gid 99 as the current user (as for MNT_IGNORE_OWNERSHIP).
enum Ownership {
    static let unknownID: UInt32 = 99
}

extension ItemAttributes {
    /// st_mode with the file type bits (FSItem.h:209: "The mode of the item").
    var fullMode: UInt32 {
        let type: UInt32 = switch kind {
        case .file: UInt32(S_IFREG)
        case .directory: UInt32(S_IFDIR)
        case .symlink: UInt32(S_IFLNK)
        }
        return type | (mode & 0o7777)
    }

    /// st_flags (FSItem.h:216): hidden is the only NTFS bit with a BSD equivalent.
    var bsdFlags: UInt32 { isHidden ? UInt32(UF_HIDDEN) : 0 }

    /// Fills an FSItem.Attributes (FSItem.h:197-260). `wanted` nil = everything.
    func fill(_ out: FSItem.Attributes, wanted: FSItem.GetAttributesRequest? = nil) {
        func want(_ a: FSItem.Attribute) -> Bool { wanted?.isAttributeWanted(a) ?? true }
        if want(.type) { out.type = ItemID.fsType(kind) }
        if want(.mode) { out.mode = fullMode }
        if want(.linkCount) { out.linkCount = linkCount }
        if want(.uid) { out.uid = Ownership.unknownID }
        if want(.gid) { out.gid = Ownership.unknownID }
        if want(.flags) { out.flags = bsdFlags }
        if want(.size) { out.size = size }
        if want(.allocSize) { out.allocSize = allocSize }
        if want(.fileID) { out.fileID = ItemID.fs(ref) }
        if want(.parentID) {
            out.parentID = ItemID.fs(ref) == .rootDirectory ? .parentOfRoot : ItemID.fs(parentRef)
        }
        if want(.modifyTime) { out.modifyTime = mtime }
        if want(.changeTime) { out.changeTime = ctime }
        if want(.accessTime) { out.accessTime = atime }
        if want(.birthTime) { out.birthTime = btime }
        // FSItem.h:231: per-file override of the volume's kernel-offloaded I/O.
        // Resident, compressed, encrypted and sparse data go through read/write instead.
        if want(.inhibitKernelOffloadedIO) { out.inhibitKernelOffloadedIO = kind == .file && inhibitsKernelIO }
    }

    func fsAttributes(wanted: FSItem.GetAttributesRequest? = nil) -> FSItem.Attributes {
        let a = FSItem.Attributes()
        fill(a, wanted: wanted)
        return a
    }
}

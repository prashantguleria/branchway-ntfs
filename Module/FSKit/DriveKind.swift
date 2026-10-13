// Removable or fixed, for the group-commit interval (5 s removable, 1 s fixed).
// FSBlockDeviceResource has no removable property (FSResource.h:150-200), so this asks
// DiskArbitration about the BSD name. When the answer is unavailable the drive counts as fixed:
// the shorter interval loses less on a yank.
// UNVERIFIED until ADP: DiskArbitration answers from inside the sandboxed FSKit appex.
import DiskArbitration
import Foundation

enum DriveKind {
    static func isRemovable(bsdName: String) -> Bool {
        guard let session = DASessionCreate(kCFAllocatorDefault),
              let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName),
              let desc = DADiskCopyDescription(disk) as? [String: Any] else { return false }
        let removable = desc[kDADiskDescriptionMediaRemovableKey as String] as? Bool ?? false
        let ejectable = desc[kDADiskDescriptionMediaEjectableKey as String] as? Bool ?? false
        let isInternal = desc[kDADiskDescriptionDeviceInternalKey as String] as? Bool ?? true
        return removable || ejectable || !isInternal
    }
}

// One FSItem per NTFS MFT reference. FSItem (FSItem.h:22) is an open class with no designated
// initializer marked unavailable, so the module subclasses it to carry the engine reference.
// The volume hands out the same object for a reference until FSKit reclaims it
// (FSVolume.h:458 reclaimItem).
import FSKit

final class NTFSItem: FSItem, @unchecked Sendable {
    let ref: UInt64

    init(ref: UInt64) {
        self.ref = ref
        super.init()
    }
}

extension FSItem {
    /// The engine reference of an item this module created.
    var engineRef: UInt64? { (self as? NTFSItem)?.ref }
}

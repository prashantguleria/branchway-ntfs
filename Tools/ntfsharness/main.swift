// ntfsharness - thin Swift CLI over the C core (Engine/).
// All work happens in nh_cmd; Swift only marshals argv. This mirrors the
// shape of a future FSKit extension: Swift on top, a C core that owns
// libntfs-3g.
import Foundation

let args = Array(CommandLine.arguments.dropFirst())
if args.isEmpty || args[0] == "--version" {
    print("ntfsharness (libntfs-3g \(String(cString: nh_libntfs_version())))")
    if args.isEmpty { _ = nh_cmd(0, nil) }
    exit(0)
}
// nh_cmd may compact argv in place, so free from a separate copy.
let owned: [UnsafeMutablePointer<CChar>?] = args.map { strdup($0) }
var cargs = owned
cargs.append(nil)
let rc = cargs.withUnsafeMutableBufferPointer { buf in
    nh_cmd(Int32(args.count), buf.baseAddress)
}
for p in owned { free(p) }
exit(rc)

// The module's entry point: an ExtensionKit app extension whose FSKit file system is a unary
// file system (one resource, one volume). UnaryFileSystemExtension: FSKit Swift overlay
// (FSKit.swiftinterface, `public protocol UnaryFileSystemExtension : AppExtension`).
import ExtensionFoundation
import FSKit

@main
struct BranchwayNTFSExtension: UnaryFileSystemExtension {
    let fileSystem = NTFSFileSystem()
}

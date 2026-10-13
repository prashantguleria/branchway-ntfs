// The unary file system: probe, load/unload, check. One resource, one volume.
//
// Protocols (MacOSX27.0 SDK, deployment 15.4, so the V1 protocols):
//   FSUnaryFileSystemOperations      FSUnaryFileSystem.h:31  probe / load / unload
//   FSManageableResourceMaintenanceOperations  FSResource.h:435  startCheck 444, startFormat
//   containerStatus                  FSFileSystemBase.h:54-60
//
// Where the journal is replayed. The brief says "Load: replay the journal first". FSKit runs a
// check before it mounts a block-device file system (FSResource.h:144: "The system doesn't mount
// block device file systems until they pass a file system check"), and FSKit's check runs after
// loadResource (startCheck has no resource parameter). So:
//   loadResource  probes, keeps the resource and the load options, returns the volume (.ready);
//   startCheck    runs bw_fsck (-q flags + journal state; -y replays the journal, then walks);
//   activate      runs bw_mount, which replays the journal FIRST, before anything else touches
//                 the volume; dirty or hibernated volumes come up read-only.
// Nothing is written to the device before activate.
import FSKit
import Foundation

final class NTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations, FSManageableResourceMaintenanceOperations, @unchecked Sendable {
    // Touched only on EngineQueue.queue.
    private var resource: FSBlockDeviceResource?
    private var bridge: IOBridge?
    private var probeInfo: ProbeInfo?
    private var volume: NTFSVolume?
    private var checking = false

    override init() {
        super.init()
        ModuleLog.install()
        ModuleLog.fs.info("Branchway NTFS module loaded, engine \(Engine.version, privacy: .public)")
    }

    // MARK: probe (FSUnaryFileSystem.h:31+)

    /// `.usable` for NTFS, `.notRecognized` for anything else; never `.usableButLimited`.
    /// BitLocker and ReFS are declined explicitly so the system's own handlers keep them.
    /// UNVERIFIED until ADP: probe arbitration against Apple's read-only ntfs (FSProbeOrder in
    /// Module/Info.plist) when both answer for the same partition.
    func probeResource(resource: FSResource, replyHandler reply: @escaping @Sendable (FSProbeResult?, (any Error)?) -> Void) {
        guard let device = resource as? FSBlockDeviceResource else { reply(.notRecognized, nil); return }
        let info = EngineQueue.run { Engine.probe(IOBridge(device: FSBlockDevice(device))) }
        switch info.kind {
        case .ntfs:
            let name = info.label.isEmpty ? "Untitled" : info.label
            reply(.usable(name: name, containerID: FSContainerIdentifier(uuid: info.containerUUID)), nil)
        case .bitLocker:
            ModuleLog.fs.info("\(device.bsdName, privacy: .public): BitLocker volume, declined")
            reply(.notRecognized, nil)
        case .refs:
            ModuleLog.fs.info("\(device.bsdName, privacy: .public): ReFS volume, declined")
            reply(.notRecognized, nil)
        case .notRecognized:
            reply(.notRecognized, nil)
        }
    }

    // MARK: load / unload

    /// Options: `--rdonly` (and `-f`), FSUnaryFileSystem.h ~44: "must remember if read-only".
    func loadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping @Sendable (FSVolume?, (any Error)?) -> Void) {
        guard let device = resource as? FSBlockDeviceResource else {
            containerStatus = .blocked(status: posixError(EINVAL))
            reply(nil, posixError(EINVAL)); return
        }
        let load = LoadOptions(options.taskOptions)
        let result: Result<NTFSVolume, any Error> = EngineQueue.run {
            let bridge = IOBridge(device: FSBlockDevice(device))
            let info = Engine.probe(bridge)
            guard info.kind == .ntfs else { return .failure(posixError(EINVAL)) }
            let removable = DriveKind.isRemovable(bsdName: device.bsdName)
            let v = NTFSVolume(resource: device, bridge: bridge, probe: info, load: load,
                               removable: removable, policy: PolicyStore.appGroup())
            self.resource = device
            self.bridge = bridge
            self.probeInfo = info
            self.volume = v
            return .success(v)
        }
        switch result {
        case .success(let v):
            containerStatus = .ready
            ModuleLog.fs.info("\(device.bsdName, privacy: .public): loaded (\(load.readOnly ? "read-only requested" : "policy decides", privacy: .public))")
            reply(v, nil)
        case .failure(let e):
            containerStatus = .blocked(status: e)
            reply(nil, e)
        }
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let err: (any Error)? = EngineQueue.run {
            defer { self.volume = nil; self.bridge = nil; self.resource = nil; self.probeInfo = nil }
            return self.volume?.shutdownOnQueue()
        }
        containerStatus = .notReady(status: posixError(ENODEV))
        reply(err)
    }

    func didFinishLoading() {
        ModuleLog.fs.debug("didFinishLoading")
    }

    // MARK: check (FSResource.h:444) and format

    /// fsck options (Info.plist FSCheckOptionSyntax "nqy"): -q quick (flags + journal state),
    /// -n read-only walk, -y replay the journal then walk. -q wins.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let check = CheckOptions(options.taskOptions)
        let progress = Progress(totalUnitCount: 100)
        let state: (IOBridge, String)? = EngineQueue.run {
            guard let bridge = self.bridge, let res = self.resource, !self.checking,
                  !(self.volume?.isActiveOnQueue ?? false) else { return nil }
            self.checking = true
            return (bridge, res.bsdName)
        }
        guard let (bridge, name) = state else { throw posixError(EBUSY) }
        containerStatus = .active
        EngineQueue.queue.async {
            let outcome: (any Error)?
            do {
                let r = try Engine.check(bridge, name: name, mode: check.mode)
                task.logMessage(r.summary)
                if r.dirty { task.logMessage("The volume is marked dirty: it mounts read-only. Run chkdsk on Windows.") }
                if r.hibernated { task.logMessage("Windows is hibernated on this volume: it mounts read-only. Shut Windows down fully.") }
                outcome = r.errors > 0 ? posixError(EIO) : nil
            } catch {
                task.logMessage("Check failed: \(error)")
                outcome = posixError(EngineError.errno(of: error))
            }
            self.checking = false
            progress.completedUnitCount = 100
            self.containerStatus = .ready
            task.didComplete(error: outcome)
        }
        return progress
    }

    /// Formatting is the Tools/bwntfs-format CLI's job (mkntfs + journal init); the module
    /// never formats.
    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw posixError(ENOTSUP)
    }
}

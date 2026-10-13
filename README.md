# Branchway NTFS

Branchway NTFS reads and writes NTFS drives on macOS 15.4 and later. It is an FSKit file-system module
(`BranchwayNTFSModule.appex`) inside a small companion app ("Branchway NTFS.app"). The engine is
[libntfs-3g](https://github.com/tuxera/ntfs-3g), unmodified, with a block journal of our own (`$BwJournal`), so a
drive pulled mid-write comes back with every acknowledged change and never half a file.

## Licence

GPL-2.0. See `LICENSE` (the text from gnu.org, unedited) and `NOTICE` (third-party code: libntfs-3g,
GPL-2.0-or-later). Complete corresponding source for every build is this repository at the tagged commit plus
the pinned libntfs-3g source named in `NOTICE`.

## How Branchway relates

Branchway (the file manager) is a separate, proprietary product in its own repository. It never links,
embeds, loads or runs any code from this repository.

- **Separate process.** The module runs as an ExtensionKit extension that macOS's `fskitd` starts. Branchway
  never starts it.
- **Kernel boundary.** Branchway sees NTFS volumes only through the ordinary file-system calls (`open`, `read`,
  `stat`, ...) that the kernel serves after Disk Arbitration mounts the volume. That is the same interface
  Finder and every other app use.
- **No linking.** No library, header or source file crosses between the repositories. Branchway only:
  - detects whether the module is installed (`FSClient.fetchInstalledExtensions`, a documented FSKit API);
  - opens the System Settings switch that enables it;
  - writes the per-drive Read Only / Read & Write choice into a shared app-group defaults suite, which the
    module reads.

## Status

| Part | State |
|---|---|
| Engine (`Engine/`): libntfs-3g + journal, fail-stop, stale-journal guard | built, tested on disk images |
| Tools (`Tools/`): `ntfsharness` (bench, fsck, journal), `bwntfs-format` | built, tested on disk images |
| Module (`Module/`) + host app (`Host/`) | builds unsigned (`CODE_SIGNING_ALLOWED=NO`); unit tests run through a test host |
| Loading the module in macOS | Needs a signed build. FSKit modules require the `com.apple.developer.fskit.fsmodule` entitlement, which comes only in a provisioning profile from a paid Apple Developer Program team. You can build from this source, but a copy you sign yourself loads only with your own team profile. Behaviour that can be exercised only inside `fskitd` is marked `// UNVERIFIED until ADP:` in the code. |

## Layout

| Path | What |
|---|---|
| `Engine/` | C engine over libntfs-3g: device layer + journal (`nh_dev.c`, `nh_wal.c`), path ops, fsck, bench |
| `Module/` | FSKit module (Swift 6): probe, check, load, volume ops, kernel-offloaded I/O |
| `Host/` | "Branchway NTFS.app": status window, Open System Settings, version |
| `Tools/` | `ntfsharness` (bench, fsck CLI, journal tools), `bwntfs-format` (mkntfs + journal init) |
| `Scripts/` | `build-libntfs-3g.sh`, test fixtures and engine checks |
| `patches/` | patches to libntfs-3g (none) |

## Build

    make libntfs                                   # pinned static libntfs-3g -> build/libntfs-3g (not committed)
    make                                           # build/ntfsharness, build/bwntfs-format
    xcodegen generate                              # BranchwayNTFS.xcodeproj from project.yml
    xcodebuild -scheme BranchwayNTFS CODE_SIGNING_ALLOWED=NO build

The unsigned build proves the code compiles. It does not make a module macOS will load: loading needs the paid
Apple Developer Program provisioning profile with the FSKit entitlement, which only the signing team holds.

## Test

    make test-api                                  # engine API over image files (Tests/engine)
    python3 Scripts/ntfs2_test.py                  # engine fixes: readdir cookies, extents, write-back
    python3 Scripts/s6/failstop_test.py             # fail-stop + stale-journal guard
    make test-format                               # bwntfs-format on images (refusals + format + journal)
    Scripts/module_test_fixture.sh                 # fixture images for the module tests
    xcodebuild -scheme BranchwayNTFS CODE_SIGNING_ALLOWED=NO test   # module unit tests via the host app

Every test runs on file-backed images under `/Volumes/Media/spikes/ntfs/s6work/ntfs2/`. None mounts anything.

## bwntfs-format

    build/bwntfs-format -L LABEL [--journal-mib N] [--dry-run] TARGET

TARGET is one unmounted partition node on an external disk (`/dev/diskNsM`) or an image file. It refuses whole
disks, internal disks, symlinks, relative paths, mounted volumes and anything another process holds open; it
never picks a device on its own. It runs `mkntfs -Q -L`, creates `/$BwJournal`, and checks the result.

## Safety rules for this repository

- Tests run on file-backed images under `/Volumes/Media/spikes/ntfs/` only. The engine refuses any other path
  (`NH_ALLOWED_ROOT`) unless a device is named explicitly in `NH_ALLOW_DEVICE`.
- `Scripts/s6/s6.py` and `UNPLUG_TEST.txt` drove a real USB stick during the spike. They are kept as evidence.
  Only the drive's owner runs them, by hand.

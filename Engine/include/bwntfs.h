/*
 * bwntfs.h - the Branchway NTFS engine API used by the FSKit module (Module/).
 *
 * The engine (libntfs-3g + the /$BwJournal journaled device layer) never opens a path in this
 * API: every byte goes through the caller's bw_io callbacks. In the module they are backed by
 * FSBlockDeviceResource (synchronous read/write, barrier = metadataFlush()); in the tests they
 * are backed by an image file.
 *
 * Threading: libntfs-3g and the engine are single-threaded. The caller serializes EVERY call
 * (all volumes included) on one queue. Nothing here blocks on anything but the bw_io callbacks.
 *
 * Errors: functions return 0 (or a count) on success and -errno on failure.
 * Item references are full NTFS MFT references (record number | sequence << 48).
 */
#ifndef BWNTFS_H
#define BWNTFS_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the device backend ---- */
typedef struct bw_io {
	void *ctx;
	/* bytes transferred, or -errno. Short transfers are allowed only at the end of the device. */
	int64_t (*pread)(void *ctx, void *buf, int64_t n, int64_t off);
	int64_t (*pwrite)(void *ctx, const void *buf, int64_t n, int64_t off);
	/* every write returned so far is on stable media when this returns 0; else -errno */
	int (*barrier)(void *ctx);
	uint64_t size;          /* device bytes */
	uint32_t block_size;    /* logical block size (512 or 4096) */
	int writable;           /* 0: the device is read-only */
} bw_io;

/* ---- log sink: libntfs-3g and engine messages (level 0 info, 1 warning, 2 error).
 * Default stderr. Engine messages written with fprintf(stderr) are not routed (CLI heritage). */
typedef void (*bw_log_fn)(int level, const char *msg);
void bw_set_log(bw_log_fn fn);

/* ---- probe (raw reads only, nothing written) ---- */
enum { BW_KIND_NOT_RECOGNIZED = 0, BW_KIND_NTFS = 1, BW_KIND_BITLOCKER = 2, BW_KIND_REFS = 3 };
typedef struct {
	int kind;
	char label[256];        /* $Volume $VOLUME_NAME, UTF-8 (empty when unset) */
	uint64_t serial;        /* boot sector volume serial number */
	uint16_t vol_flags;     /* $VOLUME_INFORMATION flags (bit 0 = dirty) */
	int dirty;              /* vol_flags & VOLUME_IS_DIRTY */
	int major_ver, minor_ver;
	uint32_t cluster_size, mft_record_size, sector_size;
	uint64_t total_bytes;   /* clusters * cluster size */
} bw_probe_info;
int bw_probe(const bw_io *io, bw_probe_info *out);

/* ---- mount ---- */
typedef struct bw_vol bw_vol;
typedef struct {
	int want_rw;            /* policy said read-write */
	int commit_ms;          /* group commit interval: 5000 removable, 1000 fixed; 0 = commit every op */
	int removable;
} bw_mount_opts;
enum {
	BW_RO_POLICY = 1,       /* the per-volume policy is read-only (or the caller asked for it) */
	BW_RO_DIRTY = 2,        /* VOLUME_IS_DIRTY: Windows did not shut down cleanly (run chkdsk) */
	BW_RO_HIBERNATED = 4,   /* hiberfil.sys in use (Windows hibernation / Fast Startup) */
	BW_RO_JOURNAL = 8,      /* /$BwJournal unusable and could not be re-created */
	BW_RO_DEVICE = 16,      /* the device is read-only */
	BW_RO_FAILED = 32,      /* fail-stop after a device error */
};
typedef struct {
	int read_only;          /* the volume is mounted read-only */
	int reasons;            /* BW_RO_* bits */
	int journal_event;      /* 0 journal as found, 1 re-created at this mount, 2 unusable */
	uint64_t replayed_txns, replayed_blocks;   /* committed transactions replayed at open */
	int stale;              /* the journal was stale (volume changed elsewhere); nothing replayed */
	uint64_t stale_txns;
} bw_mount_info;
int bw_mount(const bw_io *io, const char *name, const bw_mount_opts *opts, bw_vol **out, bw_mount_info *info);
/* commits, clears our dirty bit, closes. The handle is freed even on error. */
int bw_unmount(bw_vol *v);
/* commit now (sync, eject, the group-commit timer). Read-only volumes: 0. */
int bw_sync(bw_vol *v);
/* 1 when the commit interval elapsed since the last commit (the timer calls bw_sync then) */
int bw_commit_due(bw_vol *v);
/* 0 healthy, else the errno that stopped the volume */
int bw_failed(bw_vol *v);
int bw_read_only(bw_vol *v);

/* ---- statfs ---- */
typedef struct {
	uint32_t cluster_size;
	uint64_t total_clusters, free_clusters;
	uint64_t total_files, free_files;      /* MFT records */
	char label[256];
	int major_ver, minor_ver;
	uint64_t serial;
} bw_statfs;
int bw_statfs_get(bw_vol *v, bw_statfs *out);

/* ---- items ---- */
#define BW_ROOT_REF 5ULL                    /* the root directory's MFT record (sequence ignored) */
enum { BW_TYPE_FILE = 1, BW_TYPE_DIR = 2, BW_TYPE_SYMLINK = 3 };
enum {
	BW_F_READONLY = 1, BW_F_HIDDEN = 2, BW_F_SYSTEM = 4, BW_F_COMPRESSED = 8, BW_F_ENCRYPTED = 16,
	BW_F_SPARSE = 32, BW_F_REPARSE = 64, BW_F_ARCHIVE = 128,
	BW_F_NO_KOIO = 256,     /* resident, compressed, encrypted or sparse data: serve by read/write */
};
typedef struct { int64_t sec; int64_t nsec; } bw_time;
typedef struct {
	uint64_t ref;           /* MFT reference */
	uint64_t parent_ref;    /* first $FILE_NAME's parent (0 when unknown) */
	int type;               /* BW_TYPE_* */
	uint32_t mode;          /* POSIX permission bits synthesized like ntfs-3g (0755 / 0644, readonly drops w) */
	uint32_t nlink;
	uint32_t flags;         /* BW_F_* */
	uint32_t ntfs_attrib;   /* raw FILE_ATTR_* */
	uint64_t size, alloc_size;
	bw_time mtime, ctime, atime, btime;
} bw_attr;
int bw_getattr(bw_vol *v, uint64_t ref, bw_attr *out);
int bw_lookup(bw_vol *v, uint64_t dir, const char *name, bw_attr *out);

/* readdir: entries after `cookie` (0 = start). The callback returns nonzero to stop; the stop
 * entry is not consumed. *next receives the cookie to continue from (0 at the end). System
 * files (MFT records < 16) and /$BwJournal are hidden. "." and ".." are not reported. */
typedef int (*bw_dirent_cb)(void *ctx, const char *name, int type, uint64_t ref, uint64_t next_cookie);
int bw_readdir(bw_vol *v, uint64_t dir, uint64_t cookie, void *ctx, bw_dirent_cb cb, uint64_t *next);

int bw_create(bw_vol *v, uint64_t dir, const char *name, int type, uint32_t mode, bw_attr *out);
int bw_symlink(bw_vol *v, uint64_t dir, const char *name, const char *target, bw_attr *out);
int bw_readlink(bw_vol *v, uint64_t ref, char *buf, size_t cap);
int bw_link(bw_vol *v, uint64_t ref, uint64_t dir, const char *name);
/* removes the name `name` in `dir` (which must refer to `ref`); a directory must be empty */
int bw_remove(bw_vol *v, uint64_t dir, const char *name, uint64_t ref);
/* not atomic (libntfs-3g has no rename primitive); an existing destination is replaced
 * (an empty directory or a file) with the ntfs-3g safe-rename dance */
int bw_rename(bw_vol *v, uint64_t sdir, const char *sname, uint64_t ddir, const char *dname);

/* ---- data ---- */
/* EROFS: compressed or encrypted data opened for write (and any write on a read-only volume) */
int bw_open_check(bw_vol *v, uint64_t ref, int for_write);
int64_t bw_read(bw_vol *v, uint64_t ref, int64_t off, int64_t len, void *buf);
int64_t bw_write(bw_vol *v, uint64_t ref, int64_t off, int64_t len, const void *buf);
int bw_truncate(bw_vol *v, uint64_t ref, int64_t size);
/* times: NULL = leave unchanged */
int bw_set_times(bw_vol *v, uint64_t ref, const bw_time *mtime, const bw_time *atime, const bw_time *btime, const bw_time *ctime);
int bw_set_flags(bw_vol *v, uint64_t ref, uint32_t bw_flags_readonly_hidden);

/* ---- kernel-offloaded I/O ---- */
typedef struct { int64_t logical, physical, length; int zero_fill; } bw_extent;
/* maps [off, off+len) of the unnamed $DATA to device extents (holes and the part beyond the
 * initialized size are zero_fill). for_write: the range is allocated first (size grows to off+len).
 * Any pending journal overlay blocks inside the mapped device ranges are committed first, so
 * kernel I/O never races the overlay. -ENOTSUP for BW_F_NO_KOIO files. */
int bw_extents(bw_vol *v, uint64_t ref, int64_t off, int64_t len, int for_write, bw_extent *out, int max, int *n);
/* after a kernel write: size = max(size, end) and mtime updated */
int bw_complete_write(bw_vol *v, uint64_t ref, int64_t end);

/* ---- check ---- */
enum { BW_FSCK_QUICK = 0, BW_FSCK_CHECK = 1, BW_FSCK_REPAIR = 2 };
typedef struct {
	int errors, warnings;
	int dirty, hibernated;
	uint64_t replayed_txns, files, dirs;
	char summary[512];
} bw_fsck_result;
/* QUICK (-q): flags + journal state. CHECK: full read-only walk. REPAIR (-y): replay the journal
 * (never a stale one), then the walk. No other repairs: chkdsk on Windows owns those. */
int bw_fsck(const bw_io *io, const char *name, int mode, bw_fsck_result *out);

/* ---- format support (Tools/bwntfs-format) ----
 * Creates /$BwJournal (mib MiB, contiguous) on a freshly made NTFS volume and writes its superblock.
 * Not crash-safe on its own: run it only right after mkntfs, on a volume nobody else has open.
 * 0, or -errno (-ENOSPC when free space is too fragmented for a contiguous journal). */
int bw_journal_init(const bw_io *io, const char *name, int mib);

const char *bw_engine_version(void);
#ifdef __cplusplus
}
#endif
#endif

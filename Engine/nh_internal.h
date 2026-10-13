#ifndef NH_INTERNAL_H
#define NH_INTERNAL_H
#include "nh_config.h"
#include <CommonCrypto/CommonDigest.h>
#include "nhcore.h"
#include "bwntfs.h"
#include <time.h>

#define NH_ALLOWED_ROOT "/Volumes/Media/spikes/ntfs/"
/* S6 real media: the one device the owner allowed (SanDisk Cruzer Blade, partition 1).
 * A device path is allowed only when it is /dev/[r]diskNs1, equals NH_ALLOW_DEVICE, and
 * the opened node reports exactly NH_DEVICE_BYTES (checked by nh_open_target). */
#define NH_DEVICE_BYTES 15622733824ULL
int nh_dev_allowed(const char *path);
int nh_open_target(const char *path, int flags);   /* open an image or the allowed device (size-checked) */
#define NH_PATHMAX 8192   /* NTFS paths can exceed PATH_MAX (Windows allows 32767 units) */

/* ---- options shared by commands ---- */
typedef struct {
	const char *image;      /* --image */
	int rw;                 /* --rw */
	int nocache;            /* --nocache : F_NOCACHE on the image fd */
	int fullsync;           /* --fullsync : sync op uses F_FULLFSYNC (default) else fsync */
	const char *wlog;       /* --wlog FILE : record every device write */
	unsigned long mflags;   /* extra ntfs_mount flags */
	int quiet;
	int dirty_guard;        /* default ON (--no-dirty-guard): VOLUME_IS_DIRTY set while mounted rw */
	int ro_guard;           /* default ON (--force-dirty-rw clears): dirty/hibernated volume => read-only mount */
	int wal;                /* -1 auto (default: use /$BwJournal when present), 0 --no-wal, 1 --wal (required) */
	int commit_ms;          /* --commit op (0, default) | ms=N : group commit at most every N ms | close (-1) */
	int one_barrier;        /* --barriers adaptive : skip B1 when the transaction had no direct writes (crc guards the journal) */
	int clear_own_dirty;    /* --clear-own-dirty : a dirty bit our own journaled mount set does not force read-only */
	const bw_io *io;        /* module: callback backend instead of a path (image = a label then) */
} nh_opts;

/* ---- device (nh_dev.c) ---- */
typedef struct {
	int fd;
	const bw_io *io;        /* callback backend (fd unused, -1) */
	s64 pos;                /* seek position for the io backend */
	int nocache, fullsync, isdev, dirty_guard;
	int logfd;
	uint64_t seq;
	uint64_t nwrites, bytes_written, nreads, bytes_read, nsyncs;
	double sync_secs;
	/* journaled device layer (nh_wal.c) */
	int wal_req, wal;
	struct nh_wal *walp;
	uint32_t wal_owner_dirty_at_open;
	int commit_ms; double last_commit; int one_barrier; uint64_t wal_b1_skipped;
	uint64_t wal_commits, wal_txn_blocks, wal_jbytes, wal_direct_bytes, wal_inplace_bytes, wal_logical_meta_bytes;
	uint64_t wal_checkpoints, wal_forced_commits, wal_lib_syncs, wal_replayed_txns, wal_replayed_blocks;
	/* fail-stop: 0 healthy, else the errno that stopped the device (ENXIO/ENODEV/EIO, or a failed commit).
	 * Once set, every device read, write, barrier, mark and commit fails with it. */
	int failed;
	/* stale-journal detection (nh_wal.c): the volume stamp differed, pending transactions discarded */
	int wal_stale; uint64_t wal_stale_txns, wal_stale_blocks;
	int wal_journal_state;         /* at open: 0 usable, 1 missing, 2 invalid/truncated */
	uint64_t fault_after_writes;   /* test hook (images only): NH_FAULT_ENXIO_AFTER_WRITES=N, 0 = off */
} nh_bwdev;
s64 nh_raw_pwrite(nh_bwdev *b, const void *buf, s64 n, s64 off);
int nh_raw_barrier(nh_bwdev *b);
s64 nh_raw_pread(nh_bwdev *b, void *buf, s64 n, s64 off);
int nh_raw_mark(nh_bwdev *b, uint64_t opno);
void nh_dev_fail(nh_bwdev *b, int e);   /* enter fail-stop (first cause wins) */
int nh_wal_open(nh_bwdev *b, int ro);
int nh_wal_attach(nh_bwdev *b, ntfs_volume *vol);
s64 nh_wal_pread(nh_bwdev *b, void *buf, s64 n, s64 off);
s64 nh_wal_pwrite(nh_bwdev *b, const void *buf, s64 n, s64 off);
int nh_wal_commit(nh_bwdev *b, long long marker);
int nh_wal_set_owner_dirty(nh_bwdev *b, int v);
int nh_wal_close(nh_bwdev *b);
int nh_wal_checkpoint(nh_bwdev *b);
int nh_wal_probe(const nh_opts *o);
int nh_raw_open(nh_bwdev *b, const char *image, const bw_io *io, int flags);  /* raw access, no libntfs: 0 or -errno */
void nh_raw_close(nh_bwdev *b);
int nh_wal_range_pending(nh_bwdev *b, uint64_t off, uint64_t len);   /* 1 = overlay holds blocks in range */
int nh_fsck_vol(ntfs_volume *vol, int nodata, int *nerr, int *nwarn, uint64_t *files, uint64_t *dirs);   /* 0 usable, 1 missing, 2 invalid/truncated, <0 -errno */
int nh_journal_create(const nh_opts *o, long mib, int replace);
#define NH_JOURNAL_MIB 32
/* nh_mount's journal outcome: 0 journal usable as found, 1 re-created at this mount,
 * 2 unusable and could not be re-created (mounted read-only) */
extern int nh_journal_event;
/* group commit: a failed mutating op stops the volume (its partial changes must not ride along
 * in the next group commit). No effect on read-only, unjournaled or per-op-commit volumes. */
void nh_op_failed(ntfs_volume *vol, int err);
int nh_commit(ntfs_volume *vol, long long marker);
int cmd_jinit(int argc, char **argv);
int cmd_jinfo(int argc, char **argv);
int cmd_recover(int argc, char **argv);
ntfs_volume *nh_mount(const nh_opts *o, unsigned long flags, int *err);
int nh_umount(ntfs_volume *vol);
nh_bwdev *nh_dev_of(ntfs_volume *vol);
extern nh_bwdev nh_last_stats;
extern int nh_guard_forced_ro;    /* 1 when the last nh_mount downgraded rw to ro */   /* device counters of the last nh_umount */
int nh_wlog_marker(ntfs_volume *vol, uint64_t opno); /* 1 = this op (and all before) is now durable */
int nh_sync(ntfs_volume *vol);
int cmd_crashimg(int argc, char **argv);
int cmd_wloginfo(int argc, char **argv);

/* ---- util (nh_util.c) ---- */
double nh_now(void);
void nh_sha_hex(const unsigned char *md, char out[65]);
int nh_split(const char *path, char *parent, size_t plen, const char **leaf);
int nh_opts_parse(nh_opts *o, int *argc, char ***argv);
uint64_t nh_rand(uint64_t *s);
void nh_fill(unsigned char *buf, size_t len, uint64_t seed, uint64_t off);
ntfs_inode *nh_lookup(ntfs_volume *vol, const char *path);
int nh_uname(const char *s, ntfschar **u);
const char *nh_errstr(int e);
int nh_jprint(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* ---- file ops (nh_ops.c), path based, return 0 or -errno ---- */
int nh_create(ntfs_volume *vol, const char *path, mode_t type, ntfs_inode **out);
int nh_mkdir(ntfs_volume *vol, const char *path);
int nh_rename(ntfs_volume *vol, const char *from, const char *to);
int nh_unlink(ntfs_volume *vol, const char *path);
int nh_open_data(ntfs_volume *vol, const char *path, const char *stream, int create, ntfs_inode **ni, ntfs_attr **na);
s64 nh_pwrite_all(ntfs_attr *na, s64 off, s64 len, const void *buf);
s64 nh_pread_all(ntfs_attr *na, s64 off, s64 len, void *buf);
int nh_close_data(ntfs_inode *ni, ntfs_attr *na, int wrote);
int nh_sha_stream(ntfs_volume *vol, const char *path, const char *stream, char hex[65], s64 *size);

/* manifest (nh_bench.c) */
void nh_menc(const char *in, char *out, size_t cap);
void nh_mdec(char *s);
void nh_mopen(int fd);
void nh_mcreate(const char *path, s64 size, const char *sha);

/* commands */
int cmd_ls(int, char **);   int cmd_stat(int, char **);  int cmd_mkdir(int, char **);
int cmd_write(int, char **); int cmd_read(int, char **); int cmd_rename(int, char **);
int cmd_rm(int, char **);   int cmd_truncate(int, char **); int cmd_setattr(int, char **);
int cmd_statfs(int, char **); int cmd_flags(int, char **); int cmd_setflag(int, char **);
int cmd_probe(int, char **); int cmd_mounttest(int, char **);
int cmd_fsck(int, char **); int cmd_treegen(int, char **); int cmd_verify(int, char **);
int cmd_bench(int, char **); int cmd_sha(int, char **);
int cmd_flushbench(int, char **); int cmd_commitlat(int, char **);
#endif

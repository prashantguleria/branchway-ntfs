/*
 * nh_dev.c - "bwio" device operations for libntfs-3g.
 *
 * A file-backed image device with:
 *   - a hard guard: only image files under NH_ALLOWED_ROOT can be opened, plus
 *     (stage S6) exactly one block device, named in NH_ALLOW_DEVICE and on the
 *     compiled-in list NH_DEVICE_ALLOWLIST;
 *   - optional F_NOCACHE (cold reads) and F_FULLFSYNC on sync;
 *   - an optional write log (every pwrite + every sync barrier + op markers)
 *     that crashimg replays to build power-loss images.
 *
 * Write-log record: struct nh_wrec followed by len bytes of data (type 1 only).
 */
#include "nh_internal.h"
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/disk.h>

#define WREC_MAGIC 0x52574257u /* "BWWR" */
enum { WREC_WRITE = 1, WREC_SYNC = 2, WREC_MARK = 3 };
struct nh_wrec { uint32_t magic, type; uint64_t seq, off, len; };

#define BW(dev) ((nh_bwdev *)(dev)->d_private)

static int log_rec(nh_bwdev *b, uint32_t type, uint64_t off, uint64_t len, const void *data)
{
	struct nh_wrec r = { WREC_MAGIC, type, b->seq++, off, len };
	if (b->logfd < 0) return 0;
	if (write(b->logfd, &r, sizeof r) != sizeof r) return -1;
	if (type == WREC_WRITE && len) {
		const char *p = data; uint64_t left = len;
		while (left) { ssize_t w = write(b->logfd, p, left); if (w <= 0) return -1; p += w; left -= w; }
	}
	return 0;
}

/* ---- fail-stop ----
 * A removed or failing device (ENXIO, ENODEV, EIO) stops the volume: the first such error
 * is kept in b->failed and every later read, write, barrier, mark and commit fails with it
 * at once. The in-memory state can no longer reach the disk, so nothing may claim it did. */
void nh_dev_fail(nh_bwdev *b, int e)
{
	if (b->failed) return;
	b->failed = e ? e : EIO;
	fprintf(stderr, "device: %s; volume is fail-stop (every later write and commit fails)\n", strerror(b->failed));
}
static void dev_note(nh_bwdev *b, int e)
{
	if (e == ENXIO || e == ENODEV || e == EIO) nh_dev_fail(b, e);
}
/* 1 = refuse this I/O (errno set): the device already failed, or the test hook fires */
static int dev_stopped(nh_bwdev *b)
{
	if (!b->failed && b->fault_after_writes && b->nwrites >= b->fault_after_writes) {
		fprintf(stderr, "fault: injected ENXIO after %llu device writes\n", (unsigned long long)b->nwrites);
		nh_dev_fail(b, ENXIO);
	}
	if (b->failed) { errno = b->failed; return 1; }
	return 0;
}

s64 nh_raw_pwrite(nh_bwdev *b, const void *buf, s64 n, s64 off)
{
	if (dev_stopped(b)) return -1;
	log_rec(b, WREC_WRITE, off, n, buf);
	s64 w;
	if (b->io) { w = b->io->pwrite(b->io->ctx, buf, n, off); if (w < 0) { errno = (int)-w; w = -1; } }
	else w = pwrite(b->fd, buf, n, off);
	if (w < 0) dev_note(b, errno);
	if (w > 0) { b->nwrites++; b->bytes_written += w; }
	return w;
}
s64 nh_raw_pread(nh_bwdev *b, void *buf, s64 n, s64 off)
{
	if (dev_stopped(b)) return -1;
	s64 r;
	if (b->io) { r = b->io->pread(b->io->ctx, buf, n, off); if (r < 0) { errno = (int)-r; r = -1; } }
	else r = pread(b->fd, buf, n, off);
	if (r < 0) dev_note(b, errno);
	return r;
}
int nh_raw_barrier(nh_bwdev *b)
{
	int r = 0;
	if (dev_stopped(b)) return -1;
	double t = nh_now();
	if (b->io) {
		/* module: FSBlockDeviceResource.metadataFlush() (UNVERIFIED until ADP: reaches the media) */
		int e = b->io->barrier(b->io->ctx);
		if (e) { errno = -e; r = -1; dev_note(b, errno); }
	}
	else if (b->isdev) {
		/* device: flush the buffer cache, then ask the drive to flush its write cache */
		r = fsync(b->fd);
		if (r) dev_note(b, errno);
		if (!b->failed && b->fullsync && ioctl(b->fd, DKIOCSYNCHRONIZECACHE) == -1) {
			int e = errno;
			dev_note(b, e);
			static int warned;
			if (!warned++) fprintf(stderr, "barrier: DKIOCSYNCHRONIZECACHE failed (%s); %s\n", strerror(e), b->failed ? "device gone, no fallback" : "using F_FULLFSYNC");
			if (!b->failed && fcntl(b->fd, F_FULLFSYNC, 0) == -1 && !r) r = -1;
		}
	}
	else if (b->fullsync) { if (fcntl(b->fd, F_FULLFSYNC, 0) == -1) { r = fsync(b->fd); if (r) dev_note(b, errno); } }
	else { r = fsync(b->fd); if (r) dev_note(b, errno); }
	b->sync_secs += nh_now() - t;
	b->nsyncs++;
	log_rec(b, WREC_SYNC, 0, 0, NULL);
	if (b->failed) { errno = b->failed; return -1; }
	return r;
}
int nh_raw_mark(nh_bwdev *b, uint64_t opno)
{
	if (b->failed) { errno = b->failed; return -1; }
	return log_rec(b, WREC_MARK, opno, 0, NULL);
}

static int dv_open(struct ntfs_device *dev, int flags)
{
	nh_bwdev *b = BW(dev);
	struct flock flk;
	if (NDevOpen(dev)) { errno = EBUSY; return -1; }
	if (b->io) {
		if ((flags & O_RDWR) == O_RDWR && !b->io->writable) { errno = EROFS; return -1; }
		if ((flags & O_RDWR) != O_RDWR) NDevSetReadOnly(dev);
		b->fd = -1; b->pos = 0;
		if (b->wal_req) { int r = nh_wal_open(b, NDevReadOnly(dev)); if (r < 0) { errno = -r; return -1; } }
		NDevSetOpen(dev);
		return 0;
	}
	int isdev = nh_dev_allowed(dev->d_name);
	if (!isdev && !nh_path_allowed(dev->d_name)) {
		fprintf(stderr, "refused: %s is outside %s\n", dev->d_name, NH_ALLOWED_ROOT);
		errno = EPERM; return -1;
	}
	struct stat st;
	if (stat(dev->d_name, &st)) return -1;
	if (isdev ? !(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) : !S_ISREG(st.st_mode)) {
		fprintf(stderr, "refused: wrong file type for %s\n", dev->d_name); errno = EPERM; return -1;
	}
	b->isdev = isdev;
	b->fd = nh_open_target(dev->d_name, flags | (isdev && (flags & O_RDWR) == O_RDWR ? O_EXLOCK | O_NONBLOCK : 0));
	if (b->fd < 0) return -1;
	if (isdev) {
		fcntl(b->fd, F_SETFL, fcntl(b->fd, F_GETFL) & ~O_NONBLOCK);
		if ((flags & O_RDWR) != O_RDWR) NDevSetReadOnly(dev);
		NDevSetBlock(dev);
		if (b->wal_req) { int r = nh_wal_open(b, NDevReadOnly(dev)); if (r < 0) { close(b->fd); b->fd = -1; errno = -r; return -1; } }
		NDevSetOpen(dev); return 0;
	}
	if (b->nocache) fcntl(b->fd, F_NOCACHE, 1);
	if ((flags & O_RDWR) != O_RDWR) NDevSetReadOnly(dev);
	memset(&flk, 0, sizeof flk);
	flk.l_type = NDevReadOnly(dev) ? F_RDLCK : F_WRLCK;
	flk.l_whence = SEEK_SET;
	if (fcntl(b->fd, F_SETLK, &flk)) { int e = errno; close(b->fd); b->fd = -1; errno = e; return -1; }
	if (b->wal_req) {
		/* journal recovery happens here, before libntfs-3g reads anything */
		int r = nh_wal_open(b, NDevReadOnly(dev));
		if (r < 0) { close(b->fd); b->fd = -1; errno = -r; return -1; }
	}
	NDevSetOpen(dev);
	return 0;
}

static int dv_sync(struct ntfs_device *dev)
{
	nh_bwdev *b = BW(dev);
	if (NDevReadOnly(dev)) return 0;
	/* journaled: libntfs-3g's own sync calls fall mid-operation; only the harness commits */
	if (b->wal) { b->wal_lib_syncs++; return 0; }
	int r = nh_raw_barrier(b);
	if (!r) NDevClearDirty(dev);
	return r;
}

static int dv_close(struct ntfs_device *dev)
{
	nh_bwdev *b = BW(dev);
	if (!NDevOpen(dev)) { errno = EBADF; return -1; }
	if (b->wal) nh_wal_close(b);
	else if (NDevDirty(dev)) dv_sync(dev);
	if (b->fd >= 0) close(b->fd);
	b->fd = -1;
	NDevClearOpen(dev);
	return 0;
}

static s64 dv_seek(struct ntfs_device *dev, s64 off, int whence)
{
	nh_bwdev *b = BW(dev);
	if (!b->io) return lseek(b->fd, off, whence);
	s64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? b->pos : whence == SEEK_END ? (s64)b->io->size : -1;
	if (base < 0 || base + off < 0) { errno = EINVAL; return -1; }
	return b->pos = base + off;
}
static s64 dv_pread(struct ntfs_device *dev, void *buf, s64 n, s64 off)
{
	nh_bwdev *b = BW(dev);
	s64 r = b->wal ? nh_wal_pread(b, buf, n, off) : nh_raw_pread(b, buf, n, off);
	if (r > 0) { b->nreads++; b->bytes_read += r; }
	return r;
}
static s64 dv_pwrite(struct ntfs_device *dev, const void *buf, s64 n, s64 off)
{
	nh_bwdev *b = BW(dev);
	if (NDevReadOnly(dev)) { errno = EROFS; return -1; }
	if (b->failed) { errno = b->failed; return -1; }
	NDevSetDirty(dev);
	return b->wal ? nh_wal_pwrite(b, buf, n, off) : nh_raw_pwrite(b, buf, n, off);
}
static s64 dv_read(struct ntfs_device *dev, void *buf, s64 n)
{
	s64 pos = dv_seek(dev, 0, SEEK_CUR);
	s64 r = dv_pread(dev, buf, n, pos);
	if (r > 0) dv_seek(dev, pos + r, SEEK_SET);
	return r;
}
static s64 dv_write(struct ntfs_device *dev, const void *buf, s64 n)
{
	s64 pos = dv_seek(dev, 0, SEEK_CUR);
	s64 r = dv_pwrite(dev, buf, n, pos);
	if (r > 0) dv_seek(dev, pos + r, SEEK_SET);
	return r;
}
static int dv_stat(struct ntfs_device *dev, struct stat *st)
{
	nh_bwdev *b = BW(dev);
	if (!b->io) return fstat(b->fd, st);
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFREG | 0600; st->st_size = (off_t)b->io->size; st->st_blksize = (blksize_t)b->io->block_size;
	return 0;
}
static int dv_ioctl(struct ntfs_device *dev, unsigned long req, void *argp)
{
	nh_bwdev *b = BW(dev);
	if (b->io) {
		/* the io backend answers the two disk-geometry ioctls libntfs-3g asks for */
		if (req == DKIOCGETBLOCKSIZE && b->io->block_size) { *(uint32_t *)argp = b->io->block_size; return 0; }
		if (req == DKIOCGETBLOCKCOUNT && b->io->block_size) { *(uint64_t *)argp = b->io->size / b->io->block_size; return 0; }
		errno = ENOTTY; return -1;
	}
	/* devices: pass the disk ioctls (block count/size) through; images: none */
	if (BW(dev)->isdev) return ioctl(BW(dev)->fd, req, argp);
	(void)req; (void)argp; errno = ENOTTY; return -1;
}

static struct ntfs_device_operations bw_ops = {
	.open = dv_open, .close = dv_close, .seek = dv_seek, .read = dv_read,
	.write = dv_write, .pread = dv_pread, .pwrite = dv_pwrite, .sync = dv_sync,
	.stat = dv_stat, .ioctl = dv_ioctl,
};

int nh_guard_forced_ro;

/* raw device access without libntfs-3g (journal probe, superblock write, recover, jinfo) */
int nh_raw_open(nh_bwdev *b, const char *image, const bw_io *io, int flags)
{
	memset(b, 0, sizeof *b);
	b->fd = -1; b->logfd = -1; b->fullsync = 1; b->io = io;
	if (io) { if ((flags & O_RDWR) == O_RDWR && !io->writable) return -EROFS; return 0; }
	if (!image || (!nh_path_allowed(image) && !nh_dev_allowed(image))) return -EPERM;
	b->isdev = nh_dev_allowed(image);
	b->fd = nh_open_target(image, flags);
	return b->fd < 0 ? -errno : 0;
}
void nh_raw_close(nh_bwdev *b) { if (b->fd >= 0) close(b->fd); b->fd = -1; }

/* 0 clean, bit0 dirty, bit1 hibernated, <0 -errno. Read-only library mount, no writes. */
static int nh_guard_state(const nh_opts *o)
{
	nh_bwdev *g = calloc(1, sizeof *g);
	g->fd = -1; g->logfd = -1; g->wal_req = o->wal; g->io = o->io;
	struct ntfs_device *dev = ntfs_device_alloc(o->image, 0, &bw_ops, g);
	if (!dev) { int e = errno; free(g); return -e; }
	ntfs_volume *vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY);
	if (!vol) { int e = errno; ntfs_device_free(dev); free(g); return -e; }
	int st = (vol->flags & VOLUME_IS_DIRTY) ? 1 : 0;
	if (ntfs_volume_check_hiberfile(vol, 0) < 0 && errno == EPERM) st |= 2;
	/* the dirty bit our own journaled mount set: the journal replay restored a committed state */
	if ((st & 1) && o->clear_own_dirty && g->wal && g->wal_owner_dirty_at_open) { st &= ~1; fprintf(stderr, "guard: dirty bit is our own (journal replayed); not forcing read-only\n"); }
	ntfs_umount(vol, FALSE);
	free(g);
	return st;
}

ntfs_volume *nh_mount(const nh_opts *o, unsigned long flags, int *err)
{
	nh_bwdev *b = calloc(1, sizeof *b);
	b->fd = -1; b->logfd = -1; b->nocache = o->nocache; b->fullsync = o->fullsync; b->wal_req = o->wal; b->commit_ms = o->commit_ms; b->one_barrier = o->one_barrier; b->last_commit = nh_now(); b->io = o->io;
	if (!o->image) { *err = EINVAL; free(b); return NULL; }
	if (o->io) { if (o->wlog) { *err = EINVAL; free(b); return NULL; } goto checked; }   /* module: no path, no write log */
	/* test hook: after N device writes every device I/O fails ENXIO, as when a stick is pulled.
	 * Image files only: a real device never gets a fake fault. */
	const char *fa = getenv("NH_FAULT_ENXIO_AFTER_WRITES");
	if (fa && *fa && !nh_dev_allowed(o->image)) b->fault_after_writes = strtoull(fa, NULL, 10);
	if (!nh_path_allowed(o->image) && !nh_dev_allowed(o->image)) { fprintf(stderr, "refused: image %s outside %s\n", o->image, NH_ALLOWED_ROOT); *err = EPERM; free(b); return NULL; }
	if (o->wlog) {
		if (!nh_path_allowed(o->wlog)) { *err = EPERM; free(b); return NULL; }
		b->logfd = open(o->wlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (b->logfd < 0) { *err = errno; free(b); return NULL; }
	}
checked:;
	unsigned long f = flags | o->mflags;
	if (!o->rw) f |= NTFS_MNT_RDONLY;
	nh_guard_forced_ro = 0;
	if (o->rw && o->ro_guard) {
		/* Read-only guard: probe with a read-only mount first. A volume that is dirty
		 * (unclean unmount, Windows crash, our own crash) or hibernated (Windows
		 * hibernation / fast startup) is mounted read-only; writes then fail EROFS. */
		int st = nh_guard_state(o);
		if (st < 0) { *err = -st; if (b->logfd >= 0) close(b->logfd); free(b); return NULL; }
		if (st) {
			fprintf(stderr, "guard: volume is %s%s%s; mounting read-only\n", (st & 1) ? "dirty" : "",
			        (st & 3) == 3 ? " and " : "", (st & 2) ? "hibernated" : "");
			f |= NTFS_MNT_RDONLY; f &= ~(unsigned long)NTFS_MNT_RECOVER;
			nh_guard_forced_ro = st;
		}
	}
	nh_journal_event = 0;
	if (o->wal == 1 && !(f & NTFS_MNT_RDONLY)) {
		/* journal required and the volume is clean: a missing, truncated or broken /$BwJournal
		 * (chkdsk deletes it; a defragmenter can split it) is re-created, never a refusal */
		int js = nh_wal_probe(o);
		if (js > 0) {
			fprintf(stderr, "wal: journal %s; re-creating /$BwJournal (%d MiB)\n", js == 1 ? "missing" : "invalid or truncated", NH_JOURNAL_MIB);
			int cr = nh_journal_create(o, NH_JOURNAL_MIB, 1);
			if (cr) {
				fprintf(stderr, "wal: re-creating the journal failed (%s); mounting read-only\n", strerror(-cr));
				f |= NTFS_MNT_RDONLY; f &= ~(unsigned long)NTFS_MNT_RECOVER;
				nh_journal_event = 2;
			} else nh_journal_event = 1;
		}
	}
	struct ntfs_device *dev = ntfs_device_alloc(o->image, 0, &bw_ops, b);
	if (!dev) { *err = errno; free(b); return NULL; }
	ntfs_volume *vol = ntfs_device_mount(dev, (ntfs_mount_flags)f);
	if (!vol && o->wal == 1 && !(f & NTFS_MNT_RDONLY) && (b->wal_journal_state || errno == ENOENT || errno == EINVAL)) {
		/* the journal is still unusable: never mount read-write without it */
		fprintf(stderr, "wal: journal unusable at open (%s); mounting read-only\n", strerror(errno));
		ntfs_device_free(dev);
		f |= NTFS_MNT_RDONLY; f &= ~(unsigned long)NTFS_MNT_RECOVER;
		nh_journal_event = 2;
		b->fd = -1; b->wal_journal_state = 0;
		dev = ntfs_device_alloc(o->image, 0, &bw_ops, b);
		if (!dev) { *err = errno; free(b); return NULL; }
		vol = ntfs_device_mount(dev, (ntfs_mount_flags)f);
	}
	if (!vol) {
		*err = errno;
		if (b->logfd >= 0) close(b->logfd);
		ntfs_device_free(dev); free(b);
		return NULL;
	}
	ntfs_create_lru_caches(vol);
	if (b->wal && !NVolReadOnly(vol) && nh_wal_attach(b, vol)) fprintf(stderr, "wal: bitmap attach failed; every write is journaled\n");
	b->dirty_guard = o->dirty_guard && !NVolReadOnly(vol);
	if (b->dirty_guard) {
		/* what Windows does: the volume is dirty while mounted read-write, so a crash
		 * leaves VOLUME_IS_DIRTY set and the next mount (ours or chkdsk) knows */
		if (vol->flags & VOLUME_IS_DIRTY) fprintf(stderr, "warning: volume already dirty at mount\n");
		else if (b->wal) nh_wal_set_owner_dirty(b, 1);   /* sb before the commit that sets the bit */
		if (ntfs_volume_write_flags(vol, vol->flags | VOLUME_IS_DIRTY) || nh_sync(vol)) { *err = errno; ntfs_umount(vol, FALSE); free(b); return NULL; }
	} else if (b->wal && !NVolReadOnly(vol)) nh_sync(vol);   /* commit what the mount itself wrote */
	/* checkpoint once before any user op: the journal is empty and the superblock carries the
	 * stamp of the state after the mount's own bookkeeping */
	if (b->wal && !NVolReadOnly(vol) && nh_wal_checkpoint(b)) { *err = b->failed ? b->failed : EIO; ntfs_umount(vol, FALSE); free(b); return NULL; }
	*err = 0;
	return vol;
}

int nh_journal_event;

void nh_op_failed(ntfs_volume *vol, int err)
{
	nh_bwdev *b = BW(vol->dev);
	if (!b->wal || NVolReadOnly(vol) || b->commit_ms == 0 || b->failed) return;
	if (err < 0) err = -err;
	fprintf(stderr, "group commit: an operation failed (%s); its partial changes may be in the open group, so the volume stops "
	        "(remount to continue from the last commit)\n", strerror(err ? err : EIO));
	nh_dev_fail(b, EIO);
}

nh_bwdev nh_last_stats;
nh_bwdev *nh_dev_of(ntfs_volume *vol) { return BW(vol->dev); }

/* harness commit point: flush the library's long-lived system inodes, then commit */
int nh_commit(ntfs_volume *vol, long long marker)
{
	nh_bwdev *b = BW(vol->dev);
	if (NVolReadOnly(vol)) return 0;
	if (b->failed) { errno = b->failed; return -1; }
	ntfs_inode *sys[] = { vol->mft_ni, vol->mftmirr_ni, vol->lcnbmp_ni, vol->vol_ni };
	for (size_t i = 0; i < sizeof sys / sizeof *sys; i++)
		if (sys[i] && (NInoDirty(sys[i]) || NInoAttrListDirty(sys[i]))) ntfs_inode_sync(sys[i]);
	return nh_wal_commit(b, marker);
}

int nh_wlog_marker(ntfs_volume *vol, uint64_t opno)
{
	nh_bwdev *b = BW(vol->dev);
	if (b->wal) {
		/* op: every op durable on return; ms=N: group commit (the marker covers every op so far) */
		if (b->commit_ms < 0) return 0;
		double now = nh_now();
		if (b->commit_ms > 0 && (now - b->last_commit) * 1000 < b->commit_ms) return 0;
		int r = nh_commit(vol, (long long)opno);
		b->last_commit = nh_now();
		return r == 0;
	}
	log_rec(b, WREC_MARK, opno, 0, NULL);
	return 0;
}

int nh_sync(ntfs_volume *vol)
{
	/* libntfs-3g writes metadata at inode close/sync; make the device durable. */
	if (BW(vol->dev)->wal) return nh_commit(vol, -1);
	return ntfs_device_sync(vol->dev);
}

int nh_umount(ntfs_volume *vol)
{
	nh_bwdev *b = BW(vol->dev);
	if (b->dirty_guard) {
		/* flush everything first, then clear the bit, then flush again */
		nh_sync(vol);
		ntfs_volume_write_flags(vol, vol->flags & ~VOLUME_IS_DIRTY);
		if (b->wal) { nh_sync(vol); nh_wal_set_owner_dirty(b, 0); }
	}
	int r = ntfs_umount(vol, FALSE);
	if (b->logfd >= 0) { log_rec(b, WREC_SYNC, 0, 0, NULL); close(b->logfd); }
	nh_last_stats = *b;
	free(b);
	return r;
}

/* ---------------- write-log tools ---------------- */

typedef struct { uint32_t type; uint64_t off, len; off_t dataoff; } wrec_ix;

static wrec_ix *load_index(const char *path, size_t *n, int *fdout)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) { perror(path); return NULL; }
	size_t cap = 1024, cnt = 0;
	wrec_ix *ix = malloc(cap * sizeof *ix);
	off_t pos = 0;
	struct nh_wrec r;
	while (pread(fd, &r, sizeof r, pos) == sizeof r) {
		if (r.magic != WREC_MAGIC) { fprintf(stderr, "bad record at %lld\n", (long long)pos); break; }
		if (cnt == cap) { cap *= 2; ix = realloc(ix, cap * sizeof *ix); }
		ix[cnt].type = r.type; ix[cnt].off = r.off; ix[cnt].len = r.len;
		ix[cnt].dataoff = pos + sizeof r;
		cnt++;
		pos += sizeof r + (r.type == WREC_WRITE ? r.len : 0);
	}
	*n = cnt; *fdout = fd;
	return ix;
}

int cmd_wloginfo(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: wloginfo LOG\n"); return 2; }
	size_t n; int fd;
	wrec_ix *ix = load_index(argv[1], &n, &fd);
	if (!ix) return 1;
	uint64_t w = 0, s = 0, m = 0, bytes = 0, lastmark = 0;
	for (size_t i = 0; i < n; i++) {
		if (ix[i].type == WREC_WRITE) { w++; bytes += ix[i].len; }
		else if (ix[i].type == WREC_SYNC) s++;
		else { m++; lastmark = ix[i].off; }
	}
	printf("records=%zu writes=%llu bytes=%llu syncs=%llu markers=%llu last_op=%llu\n", n,
	       (unsigned long long)w, (unsigned long long)bytes, (unsigned long long)s,
	       (unsigned long long)m, (unsigned long long)lastmark);
	close(fd); free(ix);
	return 0;
}

/*
 * crashimg BASE LOG OUT --cut N [--reorder SEED] [--torn]
 *   N counts write records. prefix mode: the first N writes reach the disk.
 *   reorder mode: writes before the last sync barrier preceding write N are durable;
 *   each write between that barrier and N lands with probability 1/2 (a volatile
 *   cache that reorders between flushes). --torn tears the last applied write at a
 *   512-byte boundary.
 * Prints completed_op: the last op marker whose writes are all guaranteed present.
 */
int cmd_crashimg(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: crashimg BASE LOG OUT --cut N [--reorder SEED] [--torn]\n"); return 2; }
	const char *base = argv[1], *logp = argv[2], *out = argv[3];
	long long cut = -1; uint64_t seed = 0; int reorder = 0, torn = 0;
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--cut") && i + 1 < argc) cut = atoll(argv[++i]);
		else if (!strcmp(argv[i], "--reorder") && i + 1 < argc) { reorder = 1; seed = strtoull(argv[++i], NULL, 0); }
		else if (!strcmp(argv[i], "--torn")) torn = 1;
	}
	if (!nh_path_allowed(base) || !nh_path_allowed(out) || !nh_path_allowed(logp)) { fprintf(stderr, "refused: paths must be under %s\n", NH_ALLOWED_ROOT); return 1; }
	size_t n; int lfd;
	wrec_ix *ix = load_index(logp, &n, &lfd);
	if (!ix) return 1;
	/* locate the record index of write #cut */
	size_t cutrec = n; long long wseen = 0;
	for (size_t i = 0; i < n; i++) if (ix[i].type == WREC_WRITE) { if (wseen == cut) { cutrec = i; break; } wseen++; }
	size_t barrier = 0; /* records [0, barrier) are durable */
	for (size_t i = 0; i < cutrec; i++) if (ix[i].type == WREC_SYNC) barrier = i + 1;
	if (!reorder) barrier = cutrec;
	unlink(out);
	if (clonefile(base, out, 0)) { perror("clonefile"); return 1; }
	int ofd = open(out, O_RDWR);
	if (ofd < 0) { perror(out); return 1; }
	uint64_t s = seed ? seed : 1;
	size_t applied = 0, skipped = 0;
	long long last_applied = -1;
	char *buf = NULL; size_t bcap = 0;
	size_t lastw = (size_t)-1;
	for (size_t i = 0; i < cutrec; i++) if (ix[i].type == WREC_WRITE) lastw = i;
	for (size_t i = 0; i < cutrec; i++) {
		if (ix[i].type != WREC_WRITE) continue;
		if (i >= barrier && (nh_rand(&s) & 1)) { skipped++; continue; }
		if (ix[i].len > bcap) { bcap = ix[i].len; buf = realloc(buf, bcap); }
		pread(lfd, buf, ix[i].len, ix[i].dataoff);
		uint64_t len = ix[i].len;
		if (torn && i == lastw && len / 512 >= 2) len = 512 * (1 + nh_rand(&s) % (len / 512 - 1));
		pwrite(ofd, buf, len, ix[i].off);
		applied++; last_applied = (long long)i;
	}
	fsync(ofd); close(ofd);
	/* last op marker such that every write before it is present */
	uint64_t completed = 0; int have = 0;
	size_t limit = reorder ? barrier : cutrec;
	for (size_t i = 0; i < limit; i++) if (ix[i].type == WREC_MARK) { completed = ix[i].off; have = 1; }
	printf("crashimg out=%s cut=%lld writes_total=%lld applied=%zu dropped=%zu barrier_rec=%zu completed_op=%lld\n",
	       out, cut, wseen, applied, skipped, barrier, have ? (long long)completed : -1LL);
	(void)last_applied;
	free(buf); free(ix); close(lfd);
	return 0;
}

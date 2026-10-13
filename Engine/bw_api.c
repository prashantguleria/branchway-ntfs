/*
 * bw_api.c - the engine API for the FSKit module (include/bwntfs.h).
 *
 * MFT-reference based item operations over libntfs-3g, with every device byte going through
 * the caller's bw_io backend and every change through the /$BwJournal journaled device layer.
 * Modelled on the ntfs-3g FUSE driver (src/ntfs-3g.c) and nh_ops.c. Single-threaded: the
 * caller serializes every call.
 */
#include "nh_internal.h"
#include <sys/disk.h>

#define JOURNAL_NAME "$BwJournal"

struct bw_vol {
	ntfs_volume *vol;
	nh_bwdev *b;
	int reasons;
	uint64_t opno;
	char name[128];
};

const char *bw_engine_version(void) { return nh_libntfs_version(); }

int bw_journal_init(const bw_io *io, const char *name, int mib)
{
	if (!io || !io->writable) return -EROFS;
	if (mib < 1 || mib > 1024) return -EINVAL;
	bw_probe_info pi;
	int r = bw_probe(io, &pi);
	if (r) return r;
	if (pi.kind != BW_KIND_NTFS) return -EINVAL;
	nh_opts o; memset(&o, 0, sizeof o);
	o.image = name && *name ? name : "bwio"; o.io = io; o.fullsync = 1; o.wal = -1; o.quiet = 1;
	return nh_journal_create(&o, mib, 0);
}

/* libntfs-3g messages go to the caller's sink (os_log in the module), else stderr */
static bw_log_fn g_log;
static int log_handler(const char *function, const char *file, int line, u32 level, void *data, const char *format, va_list args)
{
	char msg[1024];
	int n = vsnprintf(msg, sizeof msg, format, args);
	size_t l = strlen(msg);
	while (l && msg[l - 1] == '\n') msg[--l] = 0;
	int lv = level & (NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR) ? 2 : level & NTFS_LOG_LEVEL_WARNING ? 1 : 0;
	if (level & NTFS_LOG_LEVEL_PERROR) { size_t k = strlen(msg); snprintf(msg + k, sizeof msg - k, ": %s", strerror(errno)); }
	if (g_log) g_log(lv, msg); else fprintf(stderr, "libntfs-3g: %s\n", msg);
	return n;
}
static void log_init(void)
{
	static int done;
	if (done) return;
	done = 1;
	ntfs_log_set_handler(log_handler);
	if (getenv("NH_DEBUG")) ntfs_log_set_levels(NTFS_LOG_LEVEL_DEBUG | NTFS_LOG_LEVEL_TRACE | NTFS_LOG_LEVEL_VERBOSE);
}
void bw_set_log(bw_log_fn fn) { g_log = fn; log_init(); }

/* ---------------- probe ---------------- */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }

static int io_read(const bw_io *io, void *buf, int64_t n, int64_t off)
{
	if (off < 0 || (uint64_t)(off + n) > io->size) return -EIO;
	int64_t r = io->pread(io->ctx, buf, n, off);
	return r == n ? 0 : r < 0 ? (int)r : -EIO;
}

/* applies the update sequence array of a multi-sector record; 0 or -1 */
static int mst_fixup(uint8_t *r, uint32_t size, uint32_t sector)
{
	uint16_t uo = g16(r + 4), uc = g16(r + 6);
	if (!sector || size % sector || uc != size / sector + 1 || uo + 2u * uc > size || uo & 1) return -1;
	for (uint16_t i = 1; i < uc; i++) {
		uint8_t *t = r + i * sector - 2;
		if (memcmp(t, r + uo, 2)) return -1;
		memcpy(t, r + uo + 2 * i, 2);
	}
	return 0;
}

int bw_probe(const bw_io *io, bw_probe_info *out)
{
	memset(out, 0, sizeof *out);
	log_init();
	if (!io || !io->pread || io->size < 4096) return 0;
	uint8_t bs[512];
	int e = io_read(io, bs, 512, 0);
	if (e) return e;
	if (!memcmp(bs + 3, "-FVE-FS-", 8)) { out->kind = BW_KIND_BITLOCKER; return 0; }
	if (!memcmp(bs + 3, "ReFS\0\0\0\0", 8)) { out->kind = BW_KIND_REFS; return 0; }
	if (memcmp(bs + 3, "NTFS    ", 8) || g16(bs + 510) != 0xAA55) return 0;
	uint32_t bps = g16(bs + 0x0B); uint8_t spcb = bs[0x0D];
	if (bps < 256 || bps > 4096 || (bps & (bps - 1))) return 0;
	uint32_t spc = spcb > 0x80 ? (spcb >= 0xE0 ? 1u << (256 - spcb) : 0) : spcb;
	if (!spc || (spc & (spc - 1))) return 0;
	uint64_t cs = (uint64_t)bps * spc;
	if (cs > 2u * 1024 * 1024) return 0;
	int8_t cpm = (int8_t)bs[0x40];
	uint64_t rs = cpm > 0 ? (uint64_t)cpm * cs : (cpm < -31 ? 0 : 1ull << (-cpm));
	if (rs < 1024 || rs > 65536) return 0;
	uint64_t nsect = g64(bs + 0x28), mft_lcn = g64(bs + 0x30);
	if (!nsect || mft_lcn * cs + 4 * rs > io->size) return 0;
	uint8_t *r = malloc(rs);
	if (!r) return -ENOMEM;
	e = io_read(io, r, (int64_t)rs, (int64_t)(mft_lcn * cs + 3 * rs));
	if (e) { free(r); return e; }
	if (memcmp(r, "FILE", 4) || mst_fixup(r, (uint32_t)rs, bps < 512 ? bps : 512)) { free(r); return 0; }
	out->kind = BW_KIND_NTFS;
	out->serial = g64(bs + 0x48);
	out->cluster_size = (uint32_t)cs; out->mft_record_size = (uint32_t)rs; out->sector_size = bps;
	out->total_bytes = nsect / spc * cs;
	out->major_ver = out->minor_ver = -1;
	uint32_t a = g16(r + 0x14);
	while (a + 16 <= rs) {
		uint32_t type = g32(r + a), len = g32(r + a + 4);
		if (type == 0xFFFFFFFFu || len < 16 || a + len > rs) break;
		if (!r[a + 8] && len >= 24) {   /* resident */
			uint32_t vlen = g32(r + a + 0x10); uint16_t voff = g16(r + a + 0x14);
			if (voff + (uint64_t)vlen <= len) {
				uint8_t *v = r + a + voff;
				if (type == 0x70 && vlen >= 12) { out->major_ver = v[8]; out->minor_ver = v[9]; out->vol_flags = g16(v + 10); }
				if (type == 0x60 && vlen >= 2 && vlen <= 512) {
					char *s = NULL;
					ntfschar *u = malloc(vlen); memcpy(u, v, vlen);
					if (ntfs_ucstombs(u, (int)(vlen / 2), &s, 0) > 0 && s) { strlcpy(out->label, s, sizeof out->label); }
					free(s); free(u);
				}
			}
		}
		a += len;
	}
	out->dirty = out->vol_flags & 1;
	free(r);
	return 0;
}

/* ---------------- mount ---------------- */

int bw_mount(const bw_io *io, const char *name, const bw_mount_opts *mo, bw_vol **outv, bw_mount_info *info)
{
	*outv = NULL;
	memset(info, 0, sizeof *info);
	if (!io || !io->pread || !io->pwrite || !io->barrier) return -EINVAL;
	log_init();
	bw_vol *v = calloc(1, sizeof *v);
	if (!v) return -ENOMEM;
	strlcpy(v->name, name && *name ? name : "bwio", sizeof v->name);
	int want = mo && mo->want_rw;
	if (!want) v->reasons |= BW_RO_POLICY;
	if (want && !io->writable) v->reasons |= BW_RO_DEVICE;
	nh_opts o; memset(&o, 0, sizeof o);
	o.image = v->name; o.io = io; o.fullsync = 1;
	o.rw = want && io->writable;
	o.dirty_guard = 1; o.ro_guard = 1; o.clear_own_dirty = 1;
	o.wal = o.rw ? 1 : -1;           /* read-write needs the journal; read-only replays into memory */
	o.commit_ms = mo ? mo->commit_ms : 0;
	int err = 0;
	ntfs_volume *vol = nh_mount(&o, 0, &err);
	if (!vol && o.rw && err != ENOMEM && err != ENXIO && err != ENODEV) {
		/* libntfs-3g refuses read-write on an unclean $LogFile (Windows did not shut down
		 * cleanly) and a few other states: the volume is still readable */
		fprintf(stderr, "bw_mount: read-write mount failed (%s); mounting read-only\n", strerror(err));
		v->reasons |= BW_RO_DIRTY;
		o.rw = 0; o.wal = -1;
		vol = nh_mount(&o, 0, &err);
	}
	if (!vol) { free(v); return -(err ? err : EIO); }
	if (nh_guard_forced_ro & 1) v->reasons |= BW_RO_DIRTY;
	if (nh_guard_forced_ro & 2) v->reasons |= BW_RO_HIBERNATED;
	if (nh_journal_event == 2) v->reasons |= BW_RO_JOURNAL;
	v->vol = vol; v->b = nh_dev_of(vol);
	info->read_only = NVolReadOnly(vol) ? 1 : 0;
	info->reasons = v->reasons;
	info->journal_event = nh_journal_event;
	info->replayed_txns = v->b->wal_replayed_txns; info->replayed_blocks = v->b->wal_replayed_blocks;
	info->stale = v->b->wal_stale; info->stale_txns = v->b->wal_stale_txns;
	*outv = v;
	return 0;
}

int bw_unmount(bw_vol *v)
{
	if (!v) return -EINVAL;
	int f = v->b->failed;
	int r = nh_umount(v->vol);
	free(v);
	if (f) return -f;
	return r ? -(errno ? errno : EIO) : 0;
}

int bw_read_only(bw_vol *v) { return NVolReadOnly(v->vol) || v->b->failed ? 1 : 0; }
int bw_failed(bw_vol *v) { return v->b->failed; }

int bw_sync(bw_vol *v)
{
	if (NVolReadOnly(v->vol)) return 0;
	if (v->b->failed) return -v->b->failed;
	int r = nh_sync(v->vol);
	v->b->last_commit = nh_now();
	return r ? -(v->b->failed ? v->b->failed : errno ? errno : EIO) : 0;
}

int bw_commit_due(bw_vol *v)
{
	if (NVolReadOnly(v->vol) || v->b->failed || v->b->commit_ms <= 0) return 0;
	return (nh_now() - v->b->last_commit) * 1000 >= v->b->commit_ms;
}

/* after every mutating op: an error stops a group-commit volume; success commits per policy */
static int op_end(bw_vol *v, int r)
{
	if (r) { nh_op_failed(v->vol, r); return r; }
	if (v->b->commit_ms == 0) {
		if (nh_commit(v->vol, (long long)++v->opno)) return -(v->b->failed ? v->b->failed : EIO);
		v->b->last_commit = nh_now();
	}
	return 0;
}
static int writable(bw_vol *v)
{
	if (v->b->failed) return -v->b->failed;
	if (NVolReadOnly(v->vol)) return -EROFS;
	return 0;
}

/* ---------------- statfs ---------------- */

int bw_statfs_get(bw_vol *v, bw_statfs *s)
{
	ntfs_volume *vol = v->vol;
	memset(s, 0, sizeof *s);
	if (ntfs_volume_get_free_space(vol)) return -(errno ? errno : EIO);
	s->cluster_size = vol->cluster_size;
	s->total_clusters = (uint64_t)vol->nr_clusters;
	s->free_clusters = vol->free_clusters > 0 ? (uint64_t)vol->free_clusters : 0;
	s->total_files = (uint64_t)(vol->mft_na->initialized_size >> vol->mft_record_size_bits);
	s->free_files = vol->free_mft_records > 0 ? (uint64_t)vol->free_mft_records : 0;
	/* free MFT records can also grow into free clusters */
	s->free_files += s->free_clusters * vol->cluster_size / vol->mft_record_size;
	s->total_files += s->free_clusters * vol->cluster_size / vol->mft_record_size;
	if (vol->vol_name) strlcpy(s->label, vol->vol_name, sizeof s->label);
	s->major_ver = vol->major_ver; s->minor_ver = vol->minor_ver;
	s->serial = 0;
	return 0;
}

/* ---------------- items ---------------- */

static uint64_t ref_of(ntfs_inode *ni) { return MK_MREF(ni->mft_no, le16_to_cpu(ni->mrec->sequence_number)); }

/* opens ref; a sequence number that no longer matches is ESTALE (the record was reused) */
static ntfs_inode *iopen(bw_vol *v, uint64_t ref, int *err)
{
	ntfs_inode *ni = ntfs_inode_open(v->vol, MREF(ref));
	if (!ni) { *err = -(errno ? errno : EIO); return NULL; }
	if (MSEQNO(ref) && MSEQNO(ref) != le16_to_cpu(ni->mrec->sequence_number)) { ntfs_inode_close(ni); *err = -ESTALE; return NULL; }
	if (!(ni->mrec->flags & MFT_RECORD_IN_USE)) { ntfs_inode_close(ni); *err = -ENOENT; return NULL; }
	return ni;
}
static int is_dir(ntfs_inode *ni) { return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) && !(ni->flags & FILE_ATTR_REPARSE_POINT); }

/* Interix symlink target (UTF-8, malloc'd) or NULL */
static char *intx_target(ntfs_inode *ni)
{
	if (!(ni->flags & FILE_ATTR_SYSTEM) || (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) return NULL;
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) return NULL;
	char *t = NULL;
	if (na->data_size > 8 && na->data_size <= 8 + 2 * 4096 && !(na->data_size & 1)) {
		uint8_t *b = malloc(na->data_size);
		if (b && ntfs_attr_pread(na, 0, na->data_size, b) == na->data_size && !memcmp(b, "IntxLNK\1", 8)) {
			if (ntfs_ucstombs((ntfschar *)(b + 8), (int)((na->data_size - 8) / 2), &t, 0) < 0) t = NULL;
		}
		free(b);
	}
	ntfs_attr_close(na);
	return t;
}

static void fill_attr(bw_vol *v, ntfs_inode *ni, bw_attr *a)
{
	memset(a, 0, sizeof *a);
	a->ref = ni->mft_no == FILE_root ? BW_ROOT_REF : ref_of(ni);
	u32 fl = le32_to_cpu(ni->flags);
	a->ntfs_attrib = fl;
	if (fl & FILE_ATTR_READONLY) a->flags |= BW_F_READONLY;
	if (fl & FILE_ATTR_HIDDEN) a->flags |= BW_F_HIDDEN;
	if (fl & FILE_ATTR_SYSTEM) a->flags |= BW_F_SYSTEM;
	if (fl & FILE_ATTR_COMPRESSED) a->flags |= BW_F_COMPRESSED;
	if (fl & FILE_ATTR_ENCRYPTED) a->flags |= BW_F_ENCRYPTED;
	if (fl & FILE_ATTR_SPARSE_FILE) a->flags |= BW_F_SPARSE;
	if (fl & FILE_ATTR_REPARSE_POINT) a->flags |= BW_F_REPARSE;
	if (fl & FILE_ATTR_ARCHIVE) a->flags |= BW_F_ARCHIVE;
	a->nlink = le16_to_cpu(ni->mrec->link_count);
	char *t = NULL;
	if (fl & FILE_ATTR_REPARSE_POINT) {
		a->type = BW_TYPE_SYMLINK;
		errno = 0;
		t = ntfs_make_symlink(ni, v->name);
		a->size = t ? strlen(t) : 0;
		a->flags |= BW_F_NO_KOIO;
	} else if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		a->type = BW_TYPE_DIR;
		a->nlink = 1;   /* as ntfs-3g without posix_nlink: find(1) must not prune */
		ntfs_attr *na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION, NTFS_INDEX_I30, 4);
		if (na) { a->size = (uint64_t)na->data_size; a->alloc_size = (uint64_t)na->allocated_size; ntfs_attr_close(na); }
		a->flags |= BW_F_NO_KOIO;
	} else if ((t = intx_target(ni))) {
		a->type = BW_TYPE_SYMLINK; a->size = strlen(t); a->flags |= BW_F_NO_KOIO;
	} else {
		a->type = BW_TYPE_FILE;
		ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (na) {
			a->size = (uint64_t)na->data_size;
			a->alloc_size = (uint64_t)(na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_SPARSE) ? na->compressed_size : na->allocated_size);
			if (!NAttrNonResident(na) || (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE))) a->flags |= BW_F_NO_KOIO;
			ntfs_attr_close(na);
		} else a->flags |= BW_F_NO_KOIO;
	}
	free(t);
	switch (a->type) {
	case BW_TYPE_DIR: a->mode = 0755; break;
	case BW_TYPE_SYMLINK: a->mode = 0755; break;
	default: a->mode = (fl & FILE_ATTR_READONLY) ? 0444 : 0644; break;
	}
	struct timespec ts;
	ts = ntfs2timespec(ni->last_data_change_time); a->mtime.sec = ts.tv_sec; a->mtime.nsec = ts.tv_nsec;
	ts = ntfs2timespec(ni->last_mft_change_time); a->ctime.sec = ts.tv_sec; a->ctime.nsec = ts.tv_nsec;
	ts = ntfs2timespec(ni->last_access_time); a->atime.sec = ts.tv_sec; a->atime.nsec = ts.tv_nsec;
	ts = ntfs2timespec(ni->creation_time); a->btime.sec = ts.tv_sec; a->btime.nsec = ts.tv_nsec;
	/* parent: the first $FILE_NAME */
	if (ni->mft_no != FILE_root) {
		ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
		if (ctx && !ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
			FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)ctx->attr + le16_to_cpu(ctx->attr->value_offset));
			uint64_t p = le64_to_cpu(fn->parent_directory);
			a->parent_ref = MREF(p) == FILE_root ? BW_ROOT_REF : p;
		}
		if (ctx) ntfs_attr_put_search_ctx(ctx);
	}
}

int bw_getattr(bw_vol *v, uint64_t ref, bw_attr *out)
{
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	fill_attr(v, ni, out);
	ntfs_inode_close(ni);
	return 0;
}

/* hidden from the module: system files and our journal */
static int hidden_entry(uint64_t dirmft, uint64_t mref, const ntfschar *nm, int len)
{
	if (MREF(mref) < FILE_first_user) return 1;
	if (dirmft == FILE_root && len == (int)strlen(JOURNAL_NAME)) {
		for (int i = 0; i < len; i++) if (le16_to_cpu(nm[i]) != (u16)JOURNAL_NAME[i]) return 0;
		return 1;
	}
	return 0;
}

/* case-insensitive (case-preserving) lookup of one name in an open directory */
static uint64_t dir_lookup(ntfs_volume *vol, ntfs_inode *dir, const ntfschar *u, int ulen)
{
	int cs = NVolCaseSensitive(vol);
	NVolClearCaseSensitive(vol);
	u64 r = ntfs_inode_lookup_by_name(dir, u, ulen);
	if (cs) NVolSetCaseSensitive(vol);
	return r;
}

static int to_uname(ntfs_volume *vol, const char *name, ntfschar **u, int strict)
{
	if (!name || !*name || strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..")) return -EINVAL;
	int n = nh_uname(name, u);
	if (n < 0) return n;
	if (strict && ntfs_forbidden_names(vol, *u, n, TRUE)) { free(*u); *u = NULL; return -EINVAL; }
	return n;
}

int bw_lookup(bw_vol *v, uint64_t dirref, const char *name, bw_attr *out)
{
	int e; ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) return e;
	if (!is_dir(dir)) { ntfs_inode_close(dir); return -ENOTDIR; }
	ntfschar *u; int n = to_uname(v->vol, name, &u, 0);
	if (n < 0) { ntfs_inode_close(dir); return n; }
	u64 r = dir_lookup(v->vol, dir, u, n);
	int hid = r != (u64)-1 && hidden_entry(dir->mft_no, r, u, n);
	free(u);
	ntfs_inode_close(dir);
	if (r == (u64)-1) return -(errno ? errno : ENOENT);
	if (hid) return -ENOENT;
	return bw_getattr(v, r, out);
}

struct rd_ctx { bw_vol *v; uint64_t dirmft; void *ctx; bw_dirent_cb cb; int stopped; s64 stop_pos; s64 skip_below; };
static int rd_fill(void *d, const ntfschar *name, const int len, const int ntype, const s64 pos, const MFT_REF mref, const unsigned dt)
{
	struct rd_ctx *c = d;
	if (pos < c->skip_below) return 0;
	if (ntype == FILE_NAME_DOS) return 0;
	if (len == 1 && le16_to_cpu(name[0]) == '.') return 0;
	if (len == 2 && le16_to_cpu(name[0]) == '.' && le16_to_cpu(name[1]) == '.') return 0;
	if (hidden_entry(c->dirmft, mref, name, len)) return 0;
	char *s = NULL;
	if (ntfs_ucstombs(name, len, &s, 0) < 0 || !s) return 0;   /* unconvertible name: skipped, as ntfs-3g */
	int type = dt == NTFS_DT_DIR ? BW_TYPE_DIR : dt == NTFS_DT_LNK ? BW_TYPE_SYMLINK : BW_TYPE_FILE;
	uint64_t ref = MREF(mref) == FILE_root ? BW_ROOT_REF : mref;
	int stop = c->cb(c->ctx, s, type, ref, (uint64_t)pos + 1);
	free(s);
	if (stop) { c->stopped = 1; c->stop_pos = pos; return 1; }
	return 0;
}

int bw_readdir(bw_vol *v, uint64_t dirref, uint64_t cookie, void *ctx, bw_dirent_cb cb, uint64_t *next)
{
	*next = 0;
	int e; ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) return e;
	if (!is_dir(dir)) { ntfs_inode_close(dir); return -ENOTDIR; }
	struct rd_ctx c = { v, dir->mft_no, ctx, cb, 0, 0, 0 };
	s64 pos = cookie < 2 ? 2 : (s64)cookie;   /* 0 "." and 1 ".." are never reported */
	if (pos >= v->vol->mft_record_size) {
		/* libntfs-3g 2026.9.28 dir.c ntfs_readdir() resumes in the index allocation with its
		 * bitmap cursor at bit 0 of the byte holding the resume block (bmp_buf_pos = 0, not
		 * bmp_pos & 7): resuming in any block but every 8th tests the wrong bitmap bits and
		 * reads past the allocation (EIO) or skips blocks. Resume from the first block of the
		 * bitmap byte instead and drop the entries before the cookie here. */
		u32 ibs = 0;
		ntfs_attr_search_ctx *sc = ntfs_attr_get_search_ctx(dir, NULL);
		if (sc && !ntfs_attr_lookup(AT_INDEX_ROOT, NTFS_INDEX_I30, 4, CASE_SENSITIVE, 0, NULL, 0, sc))
			ibs = le32_to_cpu(((INDEX_ROOT *)((u8 *)sc->attr + le16_to_cpu(sc->attr->value_offset)))->index_block_size);
		if (sc) ntfs_attr_put_search_ctx(sc);
		if (!ibs || (ibs & (ibs - 1))) { ntfs_inode_close(dir); return -EIO; }
		s64 ia_pos = pos - v->vol->mft_record_size;
		s64 group = (s64)ibs * 8;
		c.skip_below = pos;
		pos = v->vol->mft_record_size + ia_pos / group * group;
	}
	int r = ntfs_readdir(dir, &pos, &c, rd_fill);
	if (r && !c.stopped) { r = -(errno ? errno : EIO); ntfs_inode_close(dir); return r; }
	*next = c.stopped ? (uint64_t)c.stop_pos : 0;
	ntfs_inode_close(dir);
	return 0;
}

/* the stored name of ni in dir that matches u: an exact match first, else case-insensitive */
static int stored_name(bw_vol *v, ntfs_inode *ni, uint64_t dirmft, const ntfschar *u, int ulen, ntfschar **out, int *olen)
{
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx) return -ENOMEM;
	int found = 0;
	for (int pass = 0; pass < 2 && !found; pass++) {
		ntfs_attr_reinit_search_ctx(ctx);
		while (!found && !ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
			FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)ctx->attr + le16_to_cpu(ctx->attr->value_offset));
			if (MREF_LE(fn->parent_directory) != dirmft || fn->file_name_type == FILE_NAME_DOS) continue;
			if (ntfs_names_are_equal(fn->file_name, fn->file_name_length, u, ulen, pass ? IGNORE_CASE : CASE_SENSITIVE, v->vol->upcase, v->vol->upcase_len)) {
				*olen = fn->file_name_length;
				*out = malloc(*olen * sizeof(ntfschar) + 2);
				memcpy(*out, fn->file_name, *olen * sizeof(ntfschar));
				found = 1;
			}
		}
	}
	ntfs_attr_put_search_ctx(ctx);
	return found ? 0 : -ENOENT;
}

int bw_create(bw_vol *v, uint64_t dirref, const char *name, int type, uint32_t mode, bw_attr *out)
{
	int r = writable(v); if (r) return r;
	if (type != BW_TYPE_FILE && type != BW_TYPE_DIR) return -ENOTSUP;
	int e; ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) return e;
	if (!is_dir(dir)) { ntfs_inode_close(dir); return -ENOTDIR; }
	if (dir->mft_no == FILE_Extend) { ntfs_inode_close(dir); return -EPERM; }
	ntfschar *u; int n = to_uname(v->vol, name, &u, 1);
	if (n < 0) { ntfs_inode_close(dir); return n; }
	if (dir_lookup(v->vol, dir, u, n) != (u64)-1) { free(u); ntfs_inode_close(dir); return -EEXIST; }
	ntfs_inode *ni = ntfs_create(dir, const_cpu_to_le32(0), u, n, type == BW_TYPE_DIR ? S_IFDIR : S_IFREG);
	free(u);
	if (!ni) { r = -(errno ? errno : EIO); ntfs_inode_close(dir); return op_end(v, r); }
	if (type == BW_TYPE_FILE) {
		/* files created here start non-resident, so the kernel can map their blocks
		 * (a resident file is served by read/write; FSKit picks per file through the
		 * inhibitKernelOffloadedIO attribute). Costs one cluster per small file, as on exFAT. */
		ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (na) { if (!NAttrNonResident(na)) ntfs_attr_force_non_resident(na); ntfs_attr_close(na); }
	}
	if (type == BW_TYPE_FILE && !(mode & 0222)) {
		le32 at = ni->flags | FILE_ATTR_READONLY;
		ntfs_set_ntfs_attrib(ni, (const char *)&at, sizeof at, 0);
	}
	ntfs_inode_update_times(dir, NTFS_UPDATE_MCTIME);
	uint64_t ref = ref_of(ni);
	if (ntfs_inode_close_in_dir(ni, dir)) r = -(errno ? errno : EIO);
	if (ntfs_inode_close(dir) && !r) r = -(errno ? errno : EIO);
	r = op_end(v, r);
	if (!r && out) r = bw_getattr(v, ref, out);
	return r;
}

int bw_symlink(bw_vol *v, uint64_t dirref, const char *name, const char *target, bw_attr *out)
{
	int r = writable(v); if (r) return r;
	int e; ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) return e;
	if (!is_dir(dir)) { ntfs_inode_close(dir); return -ENOTDIR; }
	ntfschar *u, *t = NULL; int n = to_uname(v->vol, name, &u, 1);
	if (n < 0) { ntfs_inode_close(dir); return n; }
	int tn = ntfs_mbstoucs(target, &t);
	if (tn <= 0 || tn > 4095) { free(u); free(t); ntfs_inode_close(dir); return tn < 0 ? -errno : -ENAMETOOLONG; }
	if (dir_lookup(v->vol, dir, u, n) != (u64)-1) { free(u); free(t); ntfs_inode_close(dir); return -EEXIST; }
	ntfs_inode *ni = ntfs_create_symlink(dir, const_cpu_to_le32(0), u, n, t, tn);
	free(u); free(t);
	if (!ni) { r = -(errno ? errno : EIO); ntfs_inode_close(dir); return op_end(v, r); }
	ntfs_inode_update_times(dir, NTFS_UPDATE_MCTIME);
	uint64_t ref = ref_of(ni);
	if (ntfs_inode_close_in_dir(ni, dir)) r = -(errno ? errno : EIO);
	if (ntfs_inode_close(dir) && !r) r = -(errno ? errno : EIO);
	r = op_end(v, r);
	if (!r && out) r = bw_getattr(v, ref, out);
	return r;
}

int bw_readlink(bw_vol *v, uint64_t ref, char *buf, size_t cap)
{
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	char *t = (ni->flags & FILE_ATTR_REPARSE_POINT) ? ntfs_make_symlink(ni, v->name) : intx_target(ni);
	int r = 0;
	if (!t) r = (ni->flags & FILE_ATTR_REPARSE_POINT) ? -EOPNOTSUPP : -EINVAL;
	else if (strlen(t) >= cap) r = -ENAMETOOLONG;
	else { strlcpy(buf, t, cap); r = (int)strlen(t); }
	free(t);
	ntfs_inode_close(ni);
	return r;
}

/* adds name in dir for ni (both open; closes neither) */
static int link_open(bw_vol *v, ntfs_inode *ni, ntfs_inode *dir, const ntfschar *u, int n)
{
	(void)v;
	if (dir->flags & FILE_ATTR_REPARSE_POINT) return -EOPNOTSUPP;
	if (ntfs_link(ni, dir, (ntfschar *)u, (u8)n)) return -(errno ? errno : EIO);
	ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
	ntfs_inode_update_times(dir, NTFS_UPDATE_MCTIME);
	return 0;
}

static int link_refs(bw_vol *v, uint64_t ref, uint64_t dirref, const ntfschar *u, int n)
{
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) { ntfs_inode_close(ni); return e; }
	int r = is_dir(dir) ? link_open(v, ni, dir, u, n) : -ENOTDIR;
	/* parent first, so syncing ni's file name finds it in the parent index */
	if (ntfs_inode_close(dir) && !r) r = -(errno ? errno : EIO);
	if (ntfs_inode_close(ni) && !r) r = -(errno ? errno : EIO);
	return r;
}

/* removes the stored name matching u (exact first) of ref in dirref; checks emptiness */
static int unlink_refs(bw_vol *v, uint64_t dirref, const ntfschar *u, int n, uint64_t ref)
{
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	if (ni->mft_no < FILE_first_user) { ntfs_inode_close(ni); return -EPERM; }
	ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) { ntfs_inode_close(ni); return e; }
	if (!is_dir(dir)) { ntfs_inode_close(dir); ntfs_inode_close(ni); return -ENOTDIR; }
	if (dir->mft_no == FILE_Extend) { ntfs_inode_close(dir); ntfs_inode_close(ni); return -EPERM; }
	ntfschar *sn = NULL; int sl = 0;
	int r = stored_name(v, ni, dir->mft_no, u, n, &sn, &sl);
	if (r) { ntfs_inode_close(dir); ntfs_inode_close(ni); return r; }
	if (ntfs_delete(v->vol, NULL, ni, dir, sn, (u8)sl)) r = -(errno ? errno : EIO);   /* closes ni and dir */
	free(sn);
	return r;
}

int bw_link(bw_vol *v, uint64_t ref, uint64_t dirref, const char *name)
{
	int r = writable(v); if (r) return r;
	ntfschar *u; int n = to_uname(v->vol, name, &u, 1);
	if (n < 0) return n;
	int e; ntfs_inode *dir = iopen(v, dirref, &e);
	if (!dir) { free(u); return e; }
	u64 ex = is_dir(dir) ? dir_lookup(v->vol, dir, u, n) : (u64)-1;
	ntfs_inode_close(dir);
	if (ex != (u64)-1) { free(u); return -EEXIST; }
	int e2; ntfs_inode *ni = iopen(v, ref, &e2);
	if (!ni) { free(u); return e2; }
	int d = is_dir(ni); ntfs_inode_close(ni);
	if (d) { free(u); return -EPERM; }
	r = link_refs(v, ref, dirref, u, n);
	free(u);
	return op_end(v, r);
}

int bw_remove(bw_vol *v, uint64_t dirref, const char *name, uint64_t ref)
{
	int r = writable(v); if (r) return r;
	ntfschar *u; int n = to_uname(v->vol, name, &u, 0);
	if (n < 0) return n;
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) { free(u); return e; }
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		int em = ntfs_check_empty_dir(ni);
		if (em < 0) { r = -(errno ? errno : ENOTEMPTY); ntfs_inode_close(ni); free(u); return r; }
	}
	ntfs_inode_close(ni);
	r = unlink_refs(v, dirref, u, n, ref);
	free(u);
	return op_end(v, r);
}

int bw_rename(bw_vol *v, uint64_t sdir, const char *sname, uint64_t ddir, const char *dname)
{
	int r = writable(v); if (r) return r;
	ntfschar *su = NULL, *du = NULL;
	int sn = to_uname(v->vol, sname, &su, 0);
	if (sn < 0) return sn;
	int dn = to_uname(v->vol, dname, &du, 1);
	if (dn < 0) { free(su); return dn; }
	bw_attr sa, da;
	int have_dst = 0;
	r = bw_lookup(v, sdir, sname, &sa);
	if (r) goto out;
	if (sa.ref == BW_ROOT_REF || MREF(sa.ref) < FILE_first_user) { r = -EPERM; goto out; }
	int dl = bw_lookup(v, ddir, dname, &da);
	if (dl && dl != -ENOENT) { r = dl; goto out; }
	have_dst = !dl;
	if (have_dst && MREF(da.ref) == MREF(sa.ref)) {
		/* same item: a case-only rename in the same directory, else nothing to do */
		if (MREF(sdir) != MREF(ddir) || (sn == dn && !memcmp(su, du, sn * sizeof(ntfschar)))) { r = 0; goto out; }
		if ((r = link_refs(v, sa.ref, ddir, du, dn))) goto fail;
		if ((r = unlink_refs(v, sdir, su, sn, sa.ref))) goto fail;
		goto out;
	}
	if (sa.type == BW_TYPE_DIR) {
		/* a directory must not move into itself or below itself */
		uint64_t p = ddir; int guard = 0;
		while (MREF(p) != FILE_root && guard++ < 4096) {
			if (MREF(p) == MREF(sa.ref)) { r = -EINVAL; goto out; }
			bw_attr pa; if ((r = bw_getattr(v, p, &pa))) goto out;
			p = pa.parent_ref;
		}
	}
	if (have_dst) {
		if (sa.type == BW_TYPE_DIR && da.type != BW_TYPE_DIR) { r = -ENOTDIR; goto out; }
		if (sa.type != BW_TYPE_DIR && da.type == BW_TYPE_DIR) { r = -EISDIR; goto out; }
		if (da.type == BW_TYPE_DIR) {
			/* an empty directory is simply removed first */
			int e; ntfs_inode *dni = iopen(v, da.ref, &e);
			if (!dni) { r = e; goto out; }
			int em = ntfs_check_empty_dir(dni); int ee = errno;
			ntfs_inode_close(dni);
			if (em < 0) { r = -(ee ? ee : ENOTEMPTY); goto out; }
			if ((r = unlink_refs(v, ddir, du, dn, da.ref))) goto fail;
			have_dst = 0;
		}
	}
	if (have_dst) {
		/* safe-rename dance (ntfs-3g): keep the old destination under a temp name until done */
		char tmp[64]; snprintf(tmp, sizeof tmp, ".bw_rename_tmp~%llx", (unsigned long long)MREF(da.ref));
		ntfschar *tu; int tn = nh_uname(tmp, &tu);
		if (tn < 0) { r = tn; goto out; }
		if ((r = link_refs(v, da.ref, ddir, tu, tn))) { free(tu); goto fail; }
		if ((r = unlink_refs(v, ddir, du, dn, da.ref))) { unlink_refs(v, ddir, tu, tn, da.ref); free(tu); goto fail; }
		if ((r = link_refs(v, sa.ref, ddir, du, dn))) { link_refs(v, da.ref, ddir, du, dn); unlink_refs(v, ddir, tu, tn, da.ref); free(tu); goto fail; }
		if ((r = unlink_refs(v, sdir, su, sn, sa.ref))) { unlink_refs(v, ddir, du, dn, sa.ref); link_refs(v, da.ref, ddir, du, dn); unlink_refs(v, ddir, tu, tn, da.ref); free(tu); goto fail; }
		r = unlink_refs(v, ddir, tu, tn, da.ref);
		free(tu);
		if (r) goto fail;
		goto out;
	}
	if ((r = link_refs(v, sa.ref, ddir, du, dn))) goto fail;
	if ((r = unlink_refs(v, sdir, su, sn, sa.ref))) { unlink_refs(v, ddir, du, dn, sa.ref); goto fail; }
out:
	free(su); free(du);
	return r ? r : op_end(v, 0);
fail:
	free(su); free(du);
	return op_end(v, r);
}

/* ---------------- data ---------------- */

static int open_data(bw_vol *v, uint64_t ref, ntfs_inode **pni, ntfs_attr **pna)
{
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) { ntfs_inode_close(ni); return -EISDIR; }
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) { e = -(errno ? errno : EIO); ntfs_inode_close(ni); return e; }
	*pni = ni; *pna = na;
	return 0;
}
static int data_writable(ntfs_inode *ni, ntfs_attr *na)
{
	if ((ni->flags & (FILE_ATTR_ENCRYPTED | FILE_ATTR_COMPRESSED)) || (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED))) return -EROFS;
	if (ni->flags & FILE_ATTR_REPARSE_POINT) return -EROFS;
	return 0;
}
static int close_data(ntfs_inode *ni, ntfs_attr *na, int wrote)
{
	return nh_close_data(ni, na, wrote);
}

int bw_open_check(bw_vol *v, uint64_t ref, int for_write)
{
	ntfs_inode *ni; ntfs_attr *na;
	int r = open_data(v, ref, &ni, &na);
	if (r) return r;
	if (for_write) { r = writable(v); if (!r) r = data_writable(ni, na); }
	close_data(ni, na, 0);
	return r;
}

int64_t bw_read(bw_vol *v, uint64_t ref, int64_t off, int64_t len, void *buf)
{
	if (off < 0 || len < 0) return -EINVAL;
	ntfs_inode *ni; ntfs_attr *na;
	int r = open_data(v, ref, &ni, &na);
	if (r) return r;
	int64_t n = 0;
	if (off < na->data_size) {
		if (len > na->data_size - off) len = na->data_size - off;
		n = nh_pread_all(na, off, len, buf);
		if (n < 0) n = -(errno ? errno : EIO);
	}
	close_data(ni, na, 0);
	return n;
}

int64_t bw_write(bw_vol *v, uint64_t ref, int64_t off, int64_t len, const void *buf)
{
	int r = writable(v); if (r) return r;
	if (off < 0 || len < 0) return -EINVAL;
	ntfs_inode *ni; ntfs_attr *na;
	if ((r = open_data(v, ref, &ni, &na))) return r;
	if ((r = data_writable(ni, na))) { close_data(ni, na, 0); return r; }
	/* grow with real clusters first: libntfs-3g would leave a hole (sparse) for a write past
	 * the end, and sparse files lose kernel-offloaded I/O */
	if (len && off + len > na->data_size && NAttrNonResident(na) && ntfs_attr_truncate_solid(na, off + len)) {
		int e = -(errno ? errno : EIO); close_data(ni, na, 0); return op_end(v, e);
	}
	int64_t n = len ? nh_pwrite_all(na, off, len, buf) : 0;
	int e = n < 0 ? -(errno ? errno : EIO) : n < len ? -ENOSPC : 0;
	int c = close_data(ni, na, n > 0);
	if (!e && c) e = c;
	e = op_end(v, e);
	return e ? e : n;
}

int bw_truncate(bw_vol *v, uint64_t ref, int64_t size)
{
	int r = writable(v); if (r) return r;
	if (size < 0) return -EINVAL;
	ntfs_inode *ni; ntfs_attr *na;
	if ((r = open_data(v, ref, &ni, &na))) return r;
	if ((r = data_writable(ni, na))) { close_data(ni, na, 0); return r; }
	if (size != na->data_size) {
		/* growing allocates real clusters (not sparse) so the file keeps kernel-offloaded I/O;
		 * the new range reads as zeros (beyond the initialized size) */
		int wasnr = NAttrNonResident(na);
		int t = size > na->data_size ? ntfs_attr_truncate_solid(na, size) : ntfs_attr_truncate(na, size);
		if (t) r = -(errno ? errno : EIO);
		/* libntfs-3g makes a file emptied by truncate resident again: keep it mappable */
		else if (wasnr && !NAttrNonResident(na)) ntfs_attr_force_non_resident(na);
	}
	int c = close_data(ni, na, 1);
	if (!r && c) r = c;
	return op_end(v, r);
}

int bw_set_times(bw_vol *v, uint64_t ref, const bw_time *mt, const bw_time *at, const bw_time *bt, const bw_time *ct)
{
	int r = writable(v); if (r) return r;
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	struct timespec ts;
	if (mt) { ts.tv_sec = mt->sec; ts.tv_nsec = mt->nsec; ni->last_data_change_time = timespec2ntfs(ts); }
	if (at) { ts.tv_sec = at->sec; ts.tv_nsec = at->nsec; ni->last_access_time = timespec2ntfs(ts); }
	if (bt) { ts.tv_sec = bt->sec; ts.tv_nsec = bt->nsec; ni->creation_time = timespec2ntfs(ts); }
	if (ct) { ts.tv_sec = ct->sec; ts.tv_nsec = ct->nsec; ni->last_mft_change_time = timespec2ntfs(ts); }
	else ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
	NInoSetDirty(ni); NInoFileNameSetDirty(ni);
	if (ntfs_inode_close(ni)) r = -(errno ? errno : EIO);
	return op_end(v, r);
}

int bw_set_flags(bw_vol *v, uint64_t ref, uint32_t f)
{
	int r = writable(v); if (r) return r;
	int e; ntfs_inode *ni = iopen(v, ref, &e);
	if (!ni) return e;
	le32 at = ni->flags & ~(FILE_ATTR_READONLY | FILE_ATTR_HIDDEN);
	if (f & BW_F_READONLY) at |= FILE_ATTR_READONLY;
	if (f & BW_F_HIDDEN) at |= FILE_ATTR_HIDDEN;
	if (at != ni->flags && ntfs_set_ntfs_attrib(ni, (const char *)&at, sizeof at, 0)) r = -(errno ? errno : EIO);
	ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
	if (ntfs_inode_close(ni) && !r) r = -(errno ? errno : EIO);
	return op_end(v, r);
}

/* ---------------- kernel-offloaded I/O ---------------- */

static int set_initialized(ntfs_attr *na, s64 init)
{
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(na->ni, NULL);
	if (!ctx) return -ENOMEM;
	int r = 0;
	if (ntfs_attr_lookup(na->type, na->name, na->name_len, CASE_SENSITIVE, 0, NULL, 0, ctx)) r = -(errno ? errno : EIO);
	else if (ctx->attr->non_resident) {
		ctx->attr->initialized_size = cpu_to_sle64(init);
		na->initialized_size = init;
		ntfs_inode_mark_dirty(ctx->ntfs_ino);
	} else r = -EINVAL;
	ntfs_attr_put_search_ctx(ctx);
	return r;
}

/* zero-fills [from, to) through libntfs-3g (moves the initialized size up to `to`) */
static int zero_range(ntfs_attr *na, s64 from, s64 to)
{
	static unsigned char z[1 << 16];
	while (from < to) {
		s64 n = to - from > (s64)sizeof z ? (s64)sizeof z : to - from;
		if (nh_pwrite_all(na, from, n, z) != n) return -(errno ? errno : EIO);
		from += n;
	}
	return 0;
}

int bw_extents(bw_vol *v, uint64_t ref, int64_t off, int64_t len, int for_write, bw_extent *out, int max, int *n)
{
	*n = 0;
	if (off < 0 || len <= 0 || max <= 0) return -EINVAL;
	int r = 0;
	if (for_write && (r = writable(v))) return r;
	ntfs_inode *ni; ntfs_attr *na;
	if ((r = open_data(v, ref, &ni, &na))) return r;
	int wrote = 0;
	if (for_write && !NAttrNonResident(na) && !(na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED)) && !(ni->flags & FILE_ATTR_REPARSE_POINT)) {
		if (ntfs_attr_force_non_resident(na)) { r = -(errno ? errno : EIO); goto done; }
		wrote = 1;
	}
	if (!NAttrNonResident(na) || (na->data_flags & (ATTR_COMPRESSION_MASK | ATTR_IS_ENCRYPTED | ATTR_IS_SPARSE)) || (ni->flags & FILE_ATTR_REPARSE_POINT)) { r = -ENOTSUP; goto done; }
	ntfs_volume *vol = v->vol;
	s64 cs = vol->cluster_size;
	s64 end = off + len;
	if (for_write) {
		if (end > na->data_size) {
			if (ntfs_attr_truncate_solid(na, end)) { r = -(errno ? errno : EIO); goto done; }
			wrote = 1;
		}
		/* the kernel writes [off, end): anything between the initialized size and off must read
		 * as zeros afterwards, so it is zeroed now (sequential appends never take this path) */
		if (off > na->initialized_size) { if ((r = zero_range(na, na->initialized_size, off))) goto done; wrote = 1; }
	} else {
		if (off >= na->data_size) { r = 0; goto done; }
		if (end > na->data_size) end = na->data_size;
		/* the cluster holding the initialized size may carry stale bytes after it: zero them on a
		 * writable volume, else this file is served by read() */
		s64 init = na->initialized_size;
		if (init < na->data_size && init % cs) {
			if (NVolReadOnly(vol) || v->b->failed) { r = -ENOTSUP; goto done; }
			s64 to = (init + cs - 1) / cs * cs; if (to > na->data_size) to = na->data_size;
			if ((r = zero_range(na, init, to))) goto done;
			wrote = 1;
		}
	}
	if (ntfs_attr_map_whole_runlist(na)) { r = -(errno ? errno : EIO); goto done; }
	s64 init = na->initialized_size;
	s64 lo = off / cs * cs;
	int cnt = 0, pending = 0;
	for (runlist_element *rl = na->rl; rl && rl->length && cnt < max; rl++) {
		s64 rs = rl->vcn * cs, re = (rl->vcn + rl->length) * cs;
		if (re <= lo || rs >= end) continue;
		s64 s = rs > lo ? rs : lo, e = re < end ? re : end;
		/* round the tail to a cluster so whole blocks are mapped */
		e = (e + cs - 1) / cs * cs; if (e > re) e = re;
		if (rl->lcn < 0) {
			if (rl->lcn != LCN_HOLE) { r = -EIO; goto done; }
			out[cnt].logical = s; out[cnt].physical = 0; out[cnt].length = e - s; out[cnt].zero_fill = 1; cnt++;
			continue;
		}
		s64 phys = rl->lcn * cs + (s - rs);
		if (!for_write && init < e) {
			/* split at the initialized size: past it reads as zeros */
			s64 ic = init > s ? (init + cs - 1) / cs * cs : s;
			if (ic > e) ic = e;
			if (ic > s) { out[cnt] = (bw_extent){ s, phys, ic - s, 0 }; if (nh_wal_range_pending(v->b, (uint64_t)phys, (uint64_t)(ic - s))) pending = 1; cnt++; }
			if (ic < e && cnt < max) { out[cnt] = (bw_extent){ ic, 0, e - ic, 1 }; cnt++; }
			continue;
		}
		out[cnt] = (bw_extent){ s, phys, e - s, 0 };
		if (nh_wal_range_pending(v->b, (uint64_t)phys, (uint64_t)(e - s))) pending = 1;
		cnt++;
	}
	*n = cnt;
	/* kernel I/O goes straight to the device, past the journal overlay: commit what the overlay
	 * holds for these clusters first (and the allocation this call made) */
	if (pending || wrote) {
		if (!NVolReadOnly(vol)) {
			ntfs_inode_sync(ni);
			if (nh_commit(vol, -1)) r = -(v->b->failed ? v->b->failed : EIO);
			v->b->last_commit = nh_now();
		}
	}
done:
	{ int c = close_data(ni, na, 0); if (!r && c) r = c; }
	if (wrote) r = op_end(v, r);
	return r;
}

int bw_complete_write(bw_vol *v, uint64_t ref, int64_t end)
{
	int r = writable(v); if (r) return r;
	ntfs_inode *ni; ntfs_attr *na;
	if ((r = open_data(v, ref, &ni, &na))) return r;
	if (!NAttrNonResident(na)) { close_data(ni, na, 0); return -EINVAL; }
	if (end > na->data_size) end = na->data_size;
	if (end > na->initialized_size) r = set_initialized(na, end);
	int c = close_data(ni, na, 1);
	if (!r && c) r = c;
	return op_end(v, r);
}

/* ---------------- check ---------------- */

int bw_fsck(const bw_io *io, const char *name, int mode, bw_fsck_result *res)
{
	memset(res, 0, sizeof *res);
	bw_probe_info pi;
	int r = bw_probe(io, &pi);
	if (r) return r;
	if (pi.kind != BW_KIND_NTFS) { snprintf(res->summary, sizeof res->summary, "not an NTFS volume"); res->errors = 1; return -EINVAL; }
	nh_opts o; memset(&o, 0, sizeof o);
	o.image = name && *name ? name : "bwio"; o.io = io; o.fullsync = 1; o.wal = -1;
	if (mode == BW_FSCK_REPAIR && io->writable) {
		/* replay committed journal transactions in place (a stale journal is discarded) */
		nh_bwdev b;
		if ((r = nh_raw_open(&b, o.image, io, O_RDWR))) return r;
		b.wal_req = 1;
		int w = nh_wal_open(&b, 0);
		int own = 0;
		if (!w && b.walp) {
			res->replayed_txns = b.wal_replayed_txns;
			nh_wal_close(&b);
			own = 1;
		}
		nh_raw_close(&b);
		(void)own;
	}
	int err; ntfs_volume *vol = nh_mount(&o, 0, &err);
	if (!vol) { snprintf(res->summary, sizeof res->summary, "mount failed: %s", strerror(err)); res->errors = 1; return 0; }
	res->dirty = !!(vol->flags & VOLUME_IS_DIRTY);
	res->hibernated = ntfs_volume_check_hiberfile(vol, 0) < 0 && errno == EPERM;
	nh_bwdev *b = nh_dev_of(vol);
	if (mode != BW_FSCK_REPAIR) res->replayed_txns = b->wal_replayed_txns;
	if (mode != BW_FSCK_QUICK) nh_fsck_vol(vol, 1, &res->errors, &res->warnings, &res->files, &res->dirs);
	nh_umount(vol);
	snprintf(res->summary, sizeof res->summary, "%s: errors=%d warnings=%d files=%llu dirs=%llu dirty=%d hibernated=%d journal_replayed=%llu%s",
	         mode == BW_FSCK_QUICK ? "quick" : "check", res->errors, res->warnings, (unsigned long long)res->files,
	         (unsigned long long)res->dirs, res->dirty, res->hibernated, (unsigned long long)res->replayed_txns,
	         res->dirty ? " (run chkdsk on Windows to clear the dirty flag)" : "");
	return 0;
}

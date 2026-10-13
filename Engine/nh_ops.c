/*
 * nh_ops.c - path-based item operations over libntfs-3g, modelled on the
 * ntfs-3g FUSE driver (src/ntfs-3g.c), plus the simple CLI commands.
 * Every function returns 0 or -errno.
 */
#include "nh_internal.h"

static int open_parent(ntfs_volume *vol, const char *path, ntfs_inode **dir, ntfschar **u, int *ulen)
{
	char parent[NH_PATHMAX]; const char *leaf;
	int r = nh_split(path, parent, sizeof parent, &leaf);
	if (r) return r;
	*ulen = nh_uname(leaf, u);
	if (*ulen < 0) return *ulen;
	*dir = nh_lookup(vol, parent);
	if (!*dir) { r = -errno; free(*u); *u = NULL; return r; }
	return 0;
}

int nh_create(ntfs_volume *vol, const char *path, mode_t type, ntfs_inode **out)
{
	ntfs_inode *dir, *ni; ntfschar *u; int ulen, r;
	if ((r = open_parent(vol, path, &dir, &u, &ulen))) return r;
	if (dir->mft_no == FILE_Extend) { ntfs_inode_close(dir); free(u); return -EPERM; }
	ni = ntfs_create(dir, const_cpu_to_le32(0), u, ulen, type);
	if (!ni) { r = -errno; ntfs_inode_close(dir); free(u); return r; }
	ntfs_inode_update_times(dir, NTFS_UPDATE_MCTIME);
	if (out) {
		/* keep ni open; the parent index entry is refreshed when ni is synced */
		if (ntfs_inode_close(dir)) r = -errno;
		*out = ni;
	} else {
		if (ntfs_inode_close_in_dir(ni, dir)) r = -errno;
		if (ntfs_inode_close(dir)) r = -errno;
	}
	free(u);
	return r;
}

int nh_mkdir(ntfs_volume *vol, const char *path) { return nh_create(vol, path, S_IFDIR, NULL); }

int nh_unlink(ntfs_volume *vol, const char *path)
{
	ntfs_inode *ni, *dir; ntfschar *u; int ulen, r;
	ni = nh_lookup(vol, path);
	if (!ni) return -errno;
	if (ni->mft_no < FILE_first_user) { ntfs_inode_close(ni); return -EPERM; }
	if ((r = open_parent(vol, path, &dir, &u, &ulen))) { ntfs_inode_close(ni); return r; }
	if (dir->mft_no == FILE_Extend) { ntfs_inode_close(dir); ntfs_inode_close(ni); free(u); return -EPERM; }
	if (ntfs_delete(vol, path, ni, dir, u, ulen)) r = -errno; /* always closes ni and dir */
	free(u);
	return r;
}

static int do_link(ntfs_volume *vol, const char *from, const char *to)
{
	ntfs_inode *ni, *dir; ntfschar *u; int ulen, r = 0;
	ni = nh_lookup(vol, from);
	if (!ni) return -errno;
	if ((r = open_parent(vol, to, &dir, &u, &ulen))) { ntfs_inode_close(ni); return r; }
	if (dir->flags & FILE_ATTR_REPARSE_POINT) r = -EOPNOTSUPP;
	else if (ntfs_link(ni, dir, u, ulen)) r = -errno;
	else {
		ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
		ntfs_inode_update_times(dir, NTFS_UPDATE_MCTIME);
	}
	/* parent first, so syncing ni's file name finds it in the parent index */
	if (ntfs_inode_close(dir) && !r) r = -errno;
	if (ntfs_inode_close(ni) && !r) r = -errno;
	free(u);
	return r;
}

/* Not atomic: libntfs-3g has no rename primitive (same FIXME as the FUSE driver). */
int nh_rename(ntfs_volume *vol, const char *from, const char *to)
{
	ntfs_inode *dst = nh_lookup(vol, to);
	int r;
	if (dst) {
		u64 dino = dst->mft_no;
		r = ntfs_check_empty_dir(dst);
		ntfs_inode_close(dst);
		if (r < 0) return -errno;
		ntfs_inode *src = nh_lookup(vol, from);
		if (!src) return -errno;
		int same = src->mft_no == dino;
		ntfs_inode_close(src);
		if (same) return 0;
		/* safe-rename dance: keep the old destination under a temp name until done */
		char tmp[NH_PATHMAX];
		snprintf(tmp, sizeof tmp, "%s.nh_tmp~%llx", to, (unsigned long long)dino);
		if ((r = do_link(vol, to, tmp))) return r;
		if ((r = nh_unlink(vol, to))) { nh_unlink(vol, tmp); return r; }
		if ((r = do_link(vol, from, to))) { do_link(vol, tmp, to); nh_unlink(vol, tmp); return r; }
		if ((r = nh_unlink(vol, from))) { nh_unlink(vol, to); do_link(vol, tmp, to); nh_unlink(vol, tmp); return r; }
		return nh_unlink(vol, tmp);
	}
	if ((r = do_link(vol, from, to))) return r;
	if ((r = nh_unlink(vol, from))) nh_unlink(vol, to);
	return r;
}

int nh_open_data(ntfs_volume *vol, const char *path, const char *stream, int create, ntfs_inode **pni, ntfs_attr **pna)
{
	ntfs_inode *ni = nh_lookup(vol, path);
	int r;
	if (!ni) {
		if (!create || errno != ENOENT) return -errno;
		if ((r = nh_create(vol, path, S_IFREG, &ni))) return r;
	}
	ntfschar *sn = AT_UNNAMED; int slen = 0;
	if (stream && *stream) {
		slen = nh_uname(stream, &sn);
		if (slen < 0) { ntfs_inode_close(ni); return slen; }
	}
	ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, sn, slen);
	if (!na && stream && create && errno == ENOENT) {
		if (!ntfs_attr_add(ni, AT_DATA, sn, slen, NULL, 0))
			na = ntfs_attr_open(ni, AT_DATA, sn, slen);
	}
	r = na ? 0 : -errno;
	if (slen) free(sn);
	if (!na) { ntfs_inode_close(ni); return r; }
	*pni = ni; *pna = na;
	return 0;
}

s64 nh_pwrite_all(ntfs_attr *na, s64 off, s64 len, const void *buf)
{
	s64 done = 0;
	while (done < len) {
		s64 w = ntfs_attr_pwrite(na, off + done, len - done, (const char *)buf + done);
		if (w <= 0) return done ? done : -1;
		done += w;
	}
	return done;
}

s64 nh_pread_all(ntfs_attr *na, s64 off, s64 len, void *buf)
{
	s64 done = 0;
	while (done < len) {
		s64 r = ntfs_attr_pread(na, off + done, len - done, (char *)buf + done);
		if (r < 0) return -1;
		if (r == 0) break;
		done += r;
	}
	return done;
}

int nh_close_data(ntfs_inode *ni, ntfs_attr *na, int wrote)
{
	int r = 0;
	if (wrote && (na->data_flags & ATTR_COMPRESSION_MASK) && ntfs_attr_pclose(na)) r = -errno;
	ntfs_attr_close(na);
	if (wrote) {
		ni->flags |= FILE_ATTR_ARCHIVE;
		NInoFileNameSetDirty(ni);
		ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
	}
	if (ntfs_inode_close(ni) && !r) r = -errno;
	return r;
}

int nh_sha_stream(ntfs_volume *vol, const char *path, const char *stream, char hex[65], s64 *size)
{
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(vol, path, stream, 0, &ni, &na);
	if (r) return r;
	CC_SHA256_CTX c; CC_SHA256_Init(&c);
	static unsigned char buf[1 << 20];
	s64 off = 0, total = na->data_size;
	while (off < total) {
		s64 n = total - off > (s64)sizeof buf ? (s64)sizeof buf : total - off;
		s64 got = nh_pread_all(na, off, n, buf);
		if (got != n) { r = -EIO; break; }
		CC_SHA256_Update(&c, buf, (CC_LONG)n);
		off += n;
	}
	unsigned char md[32]; CC_SHA256_Final(md, &c);
	nh_sha_hex(md, hex);
	if (size) *size = total;
	nh_close_data(ni, na, 0);
	return r;
}

/* ---------------- CLI commands ---------------- */

#define MOUNT_OR_FAIL(o, vol, flags) \
	int _err; ntfs_volume *vol = nh_mount(&o, flags, &_err); \
	if (!vol) { fprintf(stderr, "mount failed: %s (errno %d)\n", strerror(_err), _err); return 3; } \
	if (o.rw && NVolReadOnly(vol)) { fprintf(stderr, "refused: volume mounted read-only by the guard: %s (errno %d)\n", strerror(EROFS), EROFS); nh_umount(vol); return 30; }

static const char *opt_val(int argc, char **argv, const char *key)
{
	for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], key)) return argv[i + 1];
	return NULL;
}
static int has_flag(int argc, char **argv, const char *key)
{
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], key)) return 1;
	return 0;
}
/* positional args, skipping "--key value" pairs */
static const char *pos_arg(int argc, char **argv, int idx)
{
	int n = 0;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--", 2)) { if (i + 1 < argc && strcmp(argv[i], "-l")) i++; continue; }
		if (!strcmp(argv[i], "-l")) continue;
		if (n++ == idx) return argv[i];
	}
	return NULL;
}

static int print_err(const char *what, int r)
{
	if (r) { fprintf(stderr, "%s: %s\n", what, nh_errstr(r)); return 1; }
	return 0;
}

struct lsctx { int longfmt; ntfs_volume *vol; int count; };
static int ls_cb(void *d, const ntfschar *name, const int len, const int type, const s64 pos, const MFT_REF mref, const unsigned dt)
{
	struct lsctx *c = d; (void)pos;
	if (type == FILE_NAME_DOS) return 0;
	char *s = NULL;
	if (ntfs_ucstombs(name, len, &s, 0) < 0) return 0;
	if (!strcmp(s, ".") || !strcmp(s, "..")) { free(s); return 0; }
	c->count++;
	if (c->longfmt) printf("%8llu %2u %s\n", (unsigned long long)MREF(mref), dt, s);
	else printf("%s\n", s);
	free(s);
	return 0;
}

int cmd_ls(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *p = pos_arg(argc, argv, 0); if (!p) p = "/";
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni = nh_lookup(vol, p);
	if (!ni) { int e = errno; nh_umount(vol); return print_err(p, -e); }
	struct lsctx c = { has_flag(argc, argv, "-l"), vol, 0 };
	s64 pos = 0;
	int r = ntfs_readdir(ni, &pos, &c, ls_cb) ? -errno : 0;
	ntfs_inode_close(ni);
	nh_umount(vol);
	fprintf(stderr, "%d entries\n", c.count);
	return print_err("readdir", r);
}

int cmd_stat(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *p = pos_arg(argc, argv, 0); if (!p) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni = nh_lookup(vol, p);
	if (!ni) { int e = errno; nh_umount(vol); return print_err(p, -e); }
	struct timespec m = ntfs2timespec(ni->last_data_change_time), c = ntfs2timespec(ni->creation_time);
	printf("inode=%llu seq=%u dir=%d flags=0x%08x links=%u data_size=%lld alloc=%lld mtime=%ld crtime=%ld attrlist=%d extents=%d\n",
	       (unsigned long long)ni->mft_no, le16_to_cpu(ni->mrec->sequence_number),
	       !!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY), le32_to_cpu(ni->flags),
	       le16_to_cpu(ni->mrec->link_count), (long long)ni->data_size, (long long)ni->allocated_size,
	       (long)m.tv_sec, (long)c.tv_sec, NInoAttrList(ni) ? 1 : 0, ni->nr_extents);
	/* list attributes */
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
	while (!ntfs_attr_lookup(AT_UNUSED, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, ctx)) {
		ATTR_RECORD *a = ctx->attr;
		char *nm = NULL;
		if (a->name_length) ntfs_ucstombs((ntfschar *)((u8 *)a + le16_to_cpu(a->name_offset)), a->name_length, &nm, 0);
		printf("  attr type=0x%x name=%s nonres=%d flags=0x%x", le32_to_cpu(a->type), nm ? nm : "", a->non_resident, le16_to_cpu(a->flags));
		if (a->non_resident) printf(" size=%lld alloc=%lld init=%lld lowvcn=%lld",
			(long long)sle64_to_cpu(a->data_size), (long long)sle64_to_cpu(a->allocated_size),
			(long long)sle64_to_cpu(a->initialized_size), (long long)sle64_to_cpu(a->lowest_vcn));
		else printf(" size=%u", le32_to_cpu(a->value_length));
		printf("\n");
		free(nm);
	}
	ntfs_attr_put_search_ctx(ctx);
	ntfs_inode_close(ni);
	nh_umount(vol);
	return 0;
}

int cmd_mkdir(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *p = pos_arg(argc, argv, 0); if (!p) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	int r = nh_mkdir(vol, p);
	nh_umount(vol);
	return print_err(p, r);
}

int cmd_write(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *src = pos_arg(argc, argv, 0), *dst = pos_arg(argc, argv, 1);
	const char *stream = opt_val(argc, argv, "--stream");
	if (!src || !dst) return 2;
	int fd = open(src, O_RDONLY); if (fd < 0) { perror(src); return 1; }
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(vol, dst, stream, 1, &ni, &na);
	if (r) { close(fd); nh_umount(vol); return print_err(dst, r); }
	if (na->data_size && ntfs_attr_truncate(na, 0)) r = -errno;
	static unsigned char buf[1 << 20];
	CC_SHA256_CTX c; CC_SHA256_Init(&c);
	s64 off = 0; ssize_t n;
	double t = nh_now();
	while (!r && (n = read(fd, buf, sizeof buf)) > 0) {
		if (nh_pwrite_all(na, off, n, buf) != n) { r = -errno ? -errno : -EIO; break; }
		CC_SHA256_Update(&c, buf, (CC_LONG)n);
		off += n;
	}
	int r2 = nh_close_data(ni, na, 1); if (!r) r = r2;
	if (!r && nh_sync(vol)) r = -errno;
	t = nh_now() - t;
	close(fd);
	unsigned char md[32]; char hex[65]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex);
	if (!r) printf("%s  %lld bytes  %.3f s\n", hex, (long long)off, t);
	if (nh_umount(vol) && !r) r = -errno;
	return print_err(dst, r);
}

int cmd_read(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *src = pos_arg(argc, argv, 0), *dst = pos_arg(argc, argv, 1);
	const char *stream = opt_val(argc, argv, "--stream");
	if (!src) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(vol, src, stream, 0, &ni, &na);
	if (r) { nh_umount(vol); return print_err(src, r); }
	int fd = -1;
	if (dst && strcmp(dst, "-")) { fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { perror(dst); r = -errno; } }
	static unsigned char buf[1 << 20];
	CC_SHA256_CTX c; CC_SHA256_Init(&c);
	s64 off = 0, total = na->data_size;
	double t = nh_now();
	while (!r && off < total) {
		s64 n = total - off > (s64)sizeof buf ? (s64)sizeof buf : total - off;
		if (nh_pread_all(na, off, n, buf) != n) { r = -EIO; break; }
		CC_SHA256_Update(&c, buf, (CC_LONG)n);
		if (fd >= 0 && write(fd, buf, n) != n) { r = -errno; break; }
		off += n;
	}
	t = nh_now() - t;
	if (fd >= 0) close(fd);
	nh_close_data(ni, na, 0);
	unsigned char md[32]; char hex[65]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex);
	if (!r) printf("%s  %lld bytes  %.3f s\n", hex, (long long)off, t);
	nh_umount(vol);
	return print_err(src, r);
}

int cmd_sha(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *src = pos_arg(argc, argv, 0);
	if (!src) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	char hex[65]; s64 sz;
	int r = nh_sha_stream(vol, src, opt_val(argc, argv, "--stream"), hex, &sz);
	if (!r) printf("%s  %lld  %s\n", hex, (long long)sz, src);
	nh_umount(vol);
	return print_err(src, r);
}

int cmd_rename(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *a = pos_arg(argc, argv, 0), *b = pos_arg(argc, argv, 1);
	if (!a || !b) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	int r = nh_rename(vol, a, b);
	if (nh_umount(vol) && !r) r = -errno;
	return print_err("rename", r);
}

int cmd_rm(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *a = pos_arg(argc, argv, 0);
	if (!a) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	int r = nh_unlink(vol, a);
	if (nh_umount(vol) && !r) r = -errno;
	return print_err(a, r);
}

int cmd_truncate(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *a = pos_arg(argc, argv, 0), *s = pos_arg(argc, argv, 1);
	if (!a || !s) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(vol, a, opt_val(argc, argv, "--stream"), 0, &ni, &na);
	if (!r) {
		if (ntfs_attr_truncate(na, atoll(s))) r = -errno;
		int r2 = nh_close_data(ni, na, 1); if (!r) r = r2;
	}
	if (nh_umount(vol) && !r) r = -errno;
	return print_err(a, r);
}

int cmd_setattr(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *a = pos_arg(argc, argv, 0);
	if (!a) return 2;
	MOUNT_OR_FAIL(o, vol, 0);
	ntfs_inode *ni = nh_lookup(vol, a);
	int r = 0;
	if (!ni) r = -errno;
	else {
		const char *v;
		if ((v = opt_val(argc, argv, "--attrib"))) {
			le32 at = cpu_to_le32((u32)strtoul(v, NULL, 16));
			if (ntfs_set_ntfs_attrib(ni, (const char *)&at, sizeof at, 0)) r = -errno;
		}
		ntfs_time_update_flags mask = 0;
		if ((v = opt_val(argc, argv, "--mtime"))) { struct timespec ts = { atol(v), 0 }; ni->last_data_change_time = timespec2ntfs(ts); }
		if ((v = opt_val(argc, argv, "--atime"))) { struct timespec ts = { atol(v), 0 }; ni->last_access_time = timespec2ntfs(ts); }
		if ((v = opt_val(argc, argv, "--crtime"))) { struct timespec ts = { atol(v), 0 }; ni->creation_time = timespec2ntfs(ts); }
		(void)mask;
		NInoSetDirty(ni); NInoFileNameSetDirty(ni);
		ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
		if (ntfs_inode_close(ni) && !r) r = -errno;
	}
	if (nh_umount(vol) && !r) r = -errno;
	return print_err(a, r);
}

int cmd_statfs(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	double t = nh_now();
	MOUNT_OR_FAIL(o, vol, 0);
	double tm = nh_now() - t;
	t = nh_now();
	ntfs_volume_get_free_space(vol);
	double tf = nh_now() - t;
	char *label = vol->vol_name;
	printf("label=%s version=%d.%d cluster=%u mft_record=%u clusters=%lld free_clusters=%lld size_bytes=%lld free_bytes=%lld mft_records=%lld free_mft_records=%lld vol_flags=0x%04x mount_ms=%.1f freespace_ms=%.1f\n",
	       label ? label : "", vol->major_ver, vol->minor_ver, vol->cluster_size, vol->mft_record_size,
	       (long long)vol->nr_clusters, (long long)vol->free_clusters,
	       (long long)vol->nr_clusters * vol->cluster_size, (long long)vol->free_clusters * vol->cluster_size,
	       (long long)(vol->mft_na->initialized_size >> vol->mft_record_size_bits), (long long)vol->free_mft_records,
	       le16_to_cpu(vol->flags), tm * 1000, tf * 1000);
	nh_umount(vol);
	return 0;
}

/* Cheap state probe through the library, read-only mount: dirty bit, hiberfile, logfile. */
int cmd_flags(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	o.rw = 0;
	double t = nh_now();
	MOUNT_OR_FAIL(o, vol, 0);
	int dirty = !!(vol->flags & VOLUME_IS_DIRTY);
	int hib = ntfs_volume_check_hiberfile(vol, 0) < 0 ? (errno == EPERM ? 1 : -1) : 0;
	nh_bwdev *b = nh_dev_of(vol);
	double ms = (nh_now() - t) * 1000;
	printf("vol_flags=0x%04x dirty=%d hibernated=%d ms=%.1f reads=%llu bytes_read=%llu\n",
	       le16_to_cpu(vol->flags), dirty, hib, ms, (unsigned long long)b->nreads, (unsigned long long)b->bytes_read);
	nh_umount(vol);
	return (dirty || hib) ? 10 : 0;
}

int cmd_setflag(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *what = pos_arg(argc, argv, 0);
	if (!what || !o.rw) { fprintf(stderr, "setflag needs --rw and dirty|clean|hiberfile|hiberfile-clear\n"); return 2; }
	/* test fixture tool: it must write exactly the flag asked for */
	o.dirty_guard = 0; o.ro_guard = 0;
	MOUNT_OR_FAIL(o, vol, NTFS_MNT_IGNORE_HIBERFILE);
	int r = 0;
	if (!strcmp(what, "dirty")) { if (ntfs_volume_write_flags(vol, vol->flags | VOLUME_IS_DIRTY)) r = -errno; }
	else if (!strcmp(what, "clean")) { if (ntfs_volume_write_flags(vol, vol->flags & ~VOLUME_IS_DIRTY)) r = -errno; }
	else if (!strcmp(what, "hiberfile") || !strcmp(what, "hiberfile-clear")) {
		/* Windows writes "hibr" (hibernated) or zeroes it on resume ("wake"); fast startup uses "HIBR" too. */
		ntfs_inode *ni; ntfs_attr *na;
		r = nh_open_data(vol, "/hiberfil.sys", NULL, 1, &ni, &na);
		if (!r) {
			static unsigned char page[65536];
			memset(page, 0, sizeof page);
			if (!strcmp(what, "hiberfile")) memcpy(page, "hibr", 4);
			if (nh_pwrite_all(na, 0, sizeof page, page) != (s64)sizeof page) r = -EIO;
			int r2 = nh_close_data(ni, na, 1); if (!r) r = r2;
		}
	} else r = -EINVAL;
	if (nh_umount(vol) && !r) r = -errno;
	return print_err(what, r);
}

/* Try the mount modes a product would try and report the outcome of each. */
int cmd_mounttest(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	struct { const char *name; int rw; unsigned long fl; } modes[] = {
		{ "rw", 1, 0 }, { "rw+may_rdonly", 1, NTFS_MNT_MAY_RDONLY }, { "rw+recover", 1, NTFS_MNT_RECOVER },
		{ "ro", 0, 0 }, { "ro+forensic", 0, NTFS_MNT_FORENSIC },
	};
	for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
		nh_opts m = o; m.rw = modes[i].rw;
		/* rw+recover would modify $LogFile; run it only when asked */
		if (modes[i].fl == NTFS_MNT_RECOVER && !has_flag(argc, argv, "--with-recover")) continue;
		int err; double t = nh_now();
		ntfs_volume *vol = nh_mount(&m, modes[i].fl, &err);
		double ms = (nh_now() - t) * 1000;
		if (!vol) { printf("mode=%s result=refused errno=%d (%s) ms=%.1f\n", modes[i].name, err, strerror(err), ms); continue; }
		printf("mode=%s result=mounted readonly=%d guard=%d vol_flags=0x%04x dirty=%d ms=%.1f\n", modes[i].name,
		       NVolReadOnly(vol) ? 1 : 0, nh_guard_forced_ro, le16_to_cpu(vol->flags), !!(vol->flags & VOLUME_IS_DIRTY), ms);
		nh_umount(vol);
	}
	return 0;
}

/*
 * probe IMG: no library mount. Reads the boot sector, the $MFT record of $Volume
 * (record 3) and returns the $VOLUME_INFORMATION flags; scans the root index root
 * for hiberfil.sys only via the library later. This is the "before anything" check.
 */
int cmd_probe(int argc, char **argv)
{
	const char *img = argc > 1 ? argv[1] : NULL;
	if (!img || (!nh_path_allowed(img) && !nh_dev_allowed(img))) { fprintf(stderr, "probe: path must be under %s\n", NH_ALLOWED_ROOT); return 2; }
	double t = nh_now();
	int fd = nh_open_target(img, O_RDONLY);
	if (fd < 0) { perror(img); return 1; }
	NTFS_BOOT_SECTOR bs;
	if (pread(fd, &bs, sizeof bs, 0) != sizeof bs || memcmp(&bs.oem_id, "NTFS    ", 8)) { printf("not ntfs\n"); close(fd); return 1; }
	u32 bps = le16_to_cpu(bs.bpb.bytes_per_sector), cs = bps * bs.bpb.sectors_per_cluster;
	s8 c = bs.clusters_per_mft_record;
	u32 recsz = c > 0 ? (u32)c * cs : 1u << (-c);
	s64 mftoff = sle64_to_cpu(bs.mft_lcn) * cs;
	unsigned char *rec = malloc(recsz);
	pread(fd, rec, recsz, mftoff + 3 * (s64)recsz);
	MFT_RECORD *m = (MFT_RECORD *)rec;
	/* apply the update sequence fixups */
	u16 usa_ofs = le16_to_cpu(m->usa_ofs), usa_count = le16_to_cpu(m->usa_count);
	u16 *usa = (u16 *)(rec + usa_ofs);
	for (u16 i = 1; i < usa_count; i++) memcpy(rec + i * 512 - 2, &usa[i], 2);
	int vflags = -1; int major = -1, minor = -1;
	ATTR_RECORD *a = (ATTR_RECORD *)(rec + le16_to_cpu(m->attrs_offset));
	while ((u8 *)a < rec + recsz && a->type != AT_END) {
		if (a->type == AT_VOLUME_INFORMATION && !a->non_resident) {
			VOLUME_INFORMATION *vi = (VOLUME_INFORMATION *)((u8 *)a + le16_to_cpu(a->value_offset));
			vflags = le16_to_cpu(vi->flags); major = vi->major_ver; minor = vi->minor_ver;
		}
		if (!a->length) break;
		a = (ATTR_RECORD *)((u8 *)a + le32_to_cpu(a->length));
	}
	double ms = (nh_now() - t) * 1000;
	printf("ntfs=1 bytes_per_sector=%u cluster=%u mft_record=%u version=%d.%d vol_flags=0x%04x dirty=%d ms=%.2f bytes_read=%u\n",
	       bps, cs, recsz, major, minor, vflags, vflags >= 0 ? !!(vflags & 1) : -1, ms, (unsigned)(sizeof bs + recsz));
	free(rec); close(fd);
	return 0;
}

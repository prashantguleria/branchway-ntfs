/*
 * bw_api_test - the module engine API (include/bwntfs.h) over an image-file bw_io.
 * Usage: bw_api_test <fresh journaled image> <scratch dir>
 * Each case prints "case NAME: PASS|FAIL detail"; the last line is "bw_api_test N/M PASS".
 */
#include "bwntfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int npass, ntotal;
#define CHECK(c, ...) do { if (!(c)) { printf("  check failed (%s:%d): %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); ok = 0; goto end; } } while (0)
#define CASE(name) static int case_##name(const char *img, const char *dir)
#define RUN(name) do { ntotal++; int r_ = case_##name(img, dir); printf("case %s: %s\n", #name, r_ ? "PASS" : "FAIL"); npass += r_; } while (0)

typedef struct { int fd; int writes; int fail_after; } fctx;
static int64_t f_pread(void *c, void *b, int64_t n, int64_t off) { ssize_t r = pread(((fctx *)c)->fd, b, (size_t)n, off); return r < 0 ? -errno : r; }
static int64_t f_pwrite(void *c, const void *b, int64_t n, int64_t off)
{
	fctx *f = c;
	if (f->fail_after >= 0 && f->writes >= f->fail_after) return -EIO;
	f->writes++;
	ssize_t r = pwrite(f->fd, b, (size_t)n, off); return r < 0 ? -errno : r;
}
static int f_barrier(void *c) { return fcntl(((fctx *)c)->fd, F_FULLFSYNC) < 0 && fsync(((fctx *)c)->fd) < 0 ? -errno : 0; }

static fctx g_ctx;
static bw_io open_io(const char *path, int writable)
{
	bw_io io; memset(&io, 0, sizeof io);
	g_ctx.fd = open(path, writable ? O_RDWR : O_RDONLY); g_ctx.writes = 0; g_ctx.fail_after = -1;
	struct stat st; fstat(g_ctx.fd, &st);
	io.ctx = &g_ctx; io.pread = f_pread; io.pwrite = f_pwrite; io.barrier = f_barrier;
	io.size = (uint64_t)st.st_size; io.block_size = 512; io.writable = writable;
	return io;
}
static void close_io(void) { if (g_ctx.fd >= 0) close(g_ctx.fd); g_ctx.fd = -1; }
static void clone(const char *a, const char *b) { char cmd[2048]; snprintf(cmd, sizeof cmd, "cp -c '%s' '%s' 2>/dev/null || cp '%s' '%s'", a, b, a, b); unlink(b); if (system(cmd)) abort(); }

static bw_vol *mount_rw(bw_io *io, int commit_ms)
{
	bw_mount_opts mo = { 1, commit_ms, 1 }; bw_mount_info mi; bw_vol *v = NULL;
	int r = bw_mount(io, "test", &mo, &v, &mi);
	if (r) { printf("  mount: %s\n", strerror(-r)); return NULL; }
	return v;
}

struct names { char n[64][256]; int c; };
static int collect(void *ctx, const char *name, int type, uint64_t ref, uint64_t next) { struct names *s = ctx; if (s->c < 64) strlcpy(s->n[s->c++], name, 256); return 0; }
static int has(struct names *s, const char *n) { for (int i = 0; i < s->c; i++) if (!strcmp(s->n[i], n)) return 1; return 0; }
static int list(bw_vol *v, uint64_t dir, struct names *s) { memset(s, 0, sizeof *s); uint64_t nx; return bw_readdir(v, dir, 0, s, collect, &nx); }

CASE(probe)
{
	int ok = 1; bw_io io = open_io(img, 0); bw_probe_info pi;
	CHECK(bw_probe(&io, &pi) == 0 && pi.kind == BW_KIND_NTFS, "kind %d", pi.kind);
	CHECK(!strcmp(pi.label, "NTFS2"), "label '%s'", pi.label);
	CHECK(pi.cluster_size == 4096 && pi.mft_record_size == 1024 && pi.major_ver == 3, "geometry %u %u %d", pi.cluster_size, pi.mft_record_size, pi.major_ver);
	CHECK(!pi.dirty && pi.serial != 0, "dirty %d", pi.dirty);
	close_io();
	/* synthetic BitLocker / ReFS / garbage */
	char p[1024]; snprintf(p, sizeof p, "%s/probe.bin", dir);
	unsigned char bs[8192]; memset(bs, 0, sizeof bs);
	memcpy(bs + 3, "-FVE-FS-", 8);
	int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644); pwrite(fd, bs, sizeof bs, 0); close(fd);
	io = open_io(p, 0); CHECK(bw_probe(&io, &pi) == 0 && pi.kind == BW_KIND_BITLOCKER, "bitlocker %d", pi.kind); close_io();
	memcpy(bs + 3, "ReFS\0\0\0\0", 8);
	fd = open(p, O_RDWR); pwrite(fd, bs, sizeof bs, 0); close(fd);
	io = open_io(p, 0); CHECK(bw_probe(&io, &pi) == 0 && pi.kind == BW_KIND_REFS, "refs %d", pi.kind); close_io();
	memcpy(bs + 3, "NTFS    ", 8); bs[510] = 0x55; bs[511] = 0xAA; bs[0x0B] = 0; bs[0x0C] = 2; bs[0x0D] = 8; bs[0x40] = 0xF6;
	bs[0x30] = 0xFF; bs[0x31] = 0xFF; bs[0x32] = 0xFF; bs[0x28] = 16; /* MFT far past the end */
	fd = open(p, O_RDWR); pwrite(fd, bs, sizeof bs, 0); close(fd);
	io = open_io(p, 0); CHECK(bw_probe(&io, &pi) == 0 && pi.kind == BW_KIND_NOT_RECOGNIZED, "bogus ntfs %d", pi.kind); close_io();
	unlink(p);
end:
	close_io(); return ok;
}

CASE(items)
{
	int ok = 1; char p[1024]; snprintf(p, sizeof p, "%s/items.img", dir); clone(img, p);
	bw_io io = open_io(p, 1); bw_vol *v = mount_rw(&io, 0); bw_attr a, b, d, t; struct names s;
	CHECK(v && !bw_read_only(v), "rw mount");
	bw_statfs sf; CHECK(bw_statfs_get(v, &sf) == 0 && sf.cluster_size == 4096 && sf.free_clusters > 0 && !strcmp(sf.label, "NTFS2"), "statfs");
	CHECK(bw_getattr(v, BW_ROOT_REF, &a) == 0 && a.type == BW_TYPE_DIR && a.ref == BW_ROOT_REF, "root");
	CHECK(list(v, BW_ROOT_REF, &s) == 0 && !has(&s, "$BwJournal") && !has(&s, "$MFT") && !has(&s, ".") && !has(&s, ".."), "root listing hides system files (%d entries)", s.c);
	CHECK(bw_create(v, BW_ROOT_REF, "Docs", BW_TYPE_DIR, 0755, &d) == 0 && d.type == BW_TYPE_DIR, "mkdir");
	CHECK(bw_create(v, d.ref, "Readme.TXT", BW_TYPE_FILE, 0644, &a) == 0 && a.type == BW_TYPE_FILE && a.size == 0 && a.parent_ref == d.ref, "create");
	CHECK(bw_create(v, d.ref, "readme.txt", BW_TYPE_FILE, 0644, NULL) == -EEXIST, "case-insensitive EEXIST");
	CHECK(bw_create(v, d.ref, "bad:name", BW_TYPE_FILE, 0644, NULL) == -EINVAL, "forbidden char");
	CHECK(bw_create(v, d.ref, "CON", BW_TYPE_FILE, 0644, NULL) == -EINVAL, "reserved name");
	CHECK(bw_lookup(v, d.ref, "README.txt", &b) == 0 && b.ref == a.ref, "case-insensitive lookup");
	CHECK(bw_lookup(v, BW_ROOT_REF, "$BwJournal", &b) == -ENOENT, "journal hidden");
	CHECK(bw_lookup(v, BW_ROOT_REF, "$MFT", &b) == -ENOENT, "system file hidden");
	/* unicode: NFC in, stored name kept */
	CHECK(bw_create(v, d.ref, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", BW_TYPE_FILE, 0644, &t) == 0, "unicode create");
	CHECK(list(v, d.ref, &s) == 0 && s.c == 2 && has(&s, "Readme.TXT"), "listing keeps case (%d)", s.c);
	/* data */
	const char *msg = "hello ntfs";
	CHECK(bw_write(v, a.ref, 0, 10, msg) == 10, "write");
	char buf[64] = {0};
	CHECK(bw_read(v, a.ref, 0, 64, buf) == 10 && !memcmp(buf, msg, 10), "read back");
	CHECK(bw_read(v, a.ref, 100, 10, buf) == 0, "read past eof");
	CHECK(bw_truncate(v, a.ref, 1 << 20) == 0 && bw_getattr(v, a.ref, &b) == 0 && b.size == 1 << 20 && !(b.flags & BW_F_SPARSE), "grow solid (flags %x)", b.flags);
	CHECK(bw_read(v, a.ref, 4096, 16, buf) == 16 && !memcmp(buf, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16), "grown range zero");
	CHECK(bw_truncate(v, a.ref, 5) == 0 && bw_getattr(v, a.ref, &b) == 0 && b.size == 5, "shrink");
	/* times + flags */
	bw_time mt = { 1700000000, 123456700 };
	CHECK(bw_set_times(v, a.ref, &mt, NULL, NULL, NULL) == 0 && bw_getattr(v, a.ref, &b) == 0 && b.mtime.sec == mt.sec && b.mtime.nsec == mt.nsec, "mtime");
	CHECK(bw_set_flags(v, a.ref, BW_F_HIDDEN) == 0 && bw_getattr(v, a.ref, &b) == 0 && (b.flags & BW_F_HIDDEN), "hidden flag");
	CHECK(bw_set_flags(v, a.ref, BW_F_READONLY) == 0 && bw_getattr(v, a.ref, &b) == 0 && (b.flags & BW_F_READONLY) && !(b.mode & 0222), "readonly flag");
	CHECK(bw_set_flags(v, a.ref, 0) == 0, "clear flags");
	/* symlink */
	CHECK(bw_symlink(v, d.ref, "link", "Readme.TXT", &b) == 0 && b.type == BW_TYPE_SYMLINK, "symlink");
	CHECK(bw_readlink(v, b.ref, buf, sizeof buf) == 10 && !strcmp(buf, "Readme.TXT"), "readlink '%s'", buf);
	/* hard link */
	CHECK(bw_link(v, a.ref, BW_ROOT_REF, "hard.txt") == 0 && bw_getattr(v, a.ref, &b) == 0 && b.nlink == 2, "link nlink %u", b.nlink);
	CHECK(bw_remove(v, BW_ROOT_REF, "hard.txt", a.ref) == 0 && bw_getattr(v, a.ref, &b) == 0 && b.nlink == 1, "unlink one name");
	/* renames */
	CHECK(bw_rename(v, d.ref, "Readme.TXT", d.ref, "README.txt") == 0 && list(v, d.ref, &s) == 0 && has(&s, "README.txt") && !has(&s, "Readme.TXT"), "case-only rename");
	CHECK(bw_rename(v, d.ref, "readme.TXT", BW_ROOT_REF, "moved.txt") == 0 && bw_lookup(v, BW_ROOT_REF, "moved.txt", &b) == 0 && b.ref == a.ref && b.parent_ref == BW_ROOT_REF, "move across dirs");
	CHECK(bw_create(v, BW_ROOT_REF, "victim.txt", BW_TYPE_FILE, 0644, &t) == 0 && bw_write(v, t.ref, 0, 3, "old") == 3, "victim");
	CHECK(bw_rename(v, BW_ROOT_REF, "moved.txt", BW_ROOT_REF, "victim.txt") == 0, "rename over file");
	CHECK(bw_lookup(v, BW_ROOT_REF, "victim.txt", &b) == 0 && b.ref == a.ref && bw_getattr(v, t.ref, &b) != 0, "replaced, old gone");
	CHECK(list(v, BW_ROOT_REF, &s) == 0 && !has(&s, "moved.txt"), "no leftover");
	for (int i = 0; i < s.c; i++) CHECK(strncmp(s.n[i], ".bw_rename_tmp", 14), "temp name left: %s", s.n[i]);
	CHECK(bw_rename(v, BW_ROOT_REF, "Docs", d.ref, "inner") == -EINVAL, "dir into itself");
	CHECK(bw_rename(v, BW_ROOT_REF, "victim.txt", BW_ROOT_REF, "Docs") == -EISDIR, "file over dir");
	CHECK(bw_remove(v, BW_ROOT_REF, "Docs", d.ref) == -ENOTEMPTY, "rmdir not empty");
	CHECK(bw_lookup(v, d.ref, "link", &b) == 0 && bw_remove(v, d.ref, "link", b.ref) == 0, "rm symlink");
	CHECK(bw_lookup(v, d.ref, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", &b) == 0 && bw_remove(v, d.ref, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", b.ref) == 0, "rm unicode");
	CHECK(bw_remove(v, BW_ROOT_REF, "docs", d.ref) == 0, "rmdir (case-insensitive name)");
	CHECK(bw_getattr(v, d.ref, &b) == -ENOENT || bw_getattr(v, d.ref, &b) == -ESTALE, "dir gone");
	/* readdir paging: cookie resumes after the last entry */
	CHECK(bw_create(v, BW_ROOT_REF, "page", BW_TYPE_DIR, 0755, &d) == 0, "page dir");
	for (int i = 0; i < 40; i++) { char n[32]; snprintf(n, sizeof n, "f%02d", i); CHECK(bw_create(v, d.ref, n, BW_TYPE_FILE, 0644, NULL) == 0, "create %s", n); }
	CHECK(list(v, d.ref, &s) == 0 && s.c == 40, "listing %d", s.c);
	CHECK(bw_sync(v) == 0, "sync");
	CHECK(bw_unmount(v) == 0, "unmount"); v = NULL; close_io();
	/* remount read-only by policy: everything is there, writes refused */
	io = open_io(p, 1);
	{ bw_mount_opts mo = { 0, 0, 1 }; bw_mount_info mi; CHECK(bw_mount(&io, "test", &mo, &v, &mi) == 0 && mi.read_only && (mi.reasons & BW_RO_POLICY), "ro mount"); }
	CHECK(bw_lookup(v, BW_ROOT_REF, "victim.txt", &b) == 0 && b.size == 5, "persisted size %llu", (unsigned long long)b.size);
	CHECK(bw_read(v, b.ref, 0, 64, buf) == 5 && !memcmp(buf, "hello", 5), "persisted data");
	CHECK(bw_create(v, BW_ROOT_REF, "x", BW_TYPE_FILE, 0644, NULL) == -EROFS, "ro create EROFS");
	CHECK(bw_write(v, b.ref, 0, 1, "x") == -EROFS, "ro write EROFS");
	CHECK(bw_unmount(v) == 0, "ro unmount"); v = NULL; close_io();
	io = open_io(p, 0);
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "test", BW_FSCK_CHECK, &fr) == 0 && fr.errors == 0 && !fr.dirty, "fsck: %s", fr.summary); }
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

static int stop_after;
static int collect_stop(void *ctx, const char *name, int type, uint64_t ref, uint64_t next) { struct names *s = ctx; if (s->c >= stop_after) return 1; strlcpy(s->n[s->c++], name, 256); return 0; }

CASE(readdir_paging)
{
	int ok = 1; char p[1024]; snprintf(p, sizeof p, "%s/page.img", dir); clone(img, p);
	bw_io io = open_io(p, 1); bw_vol *v = mount_rw(&io, 5000); bw_attr d;
	CHECK(v, "mount");
	CHECK(bw_create(v, BW_ROOT_REF, "page", BW_TYPE_DIR, 0755, &d) == 0, "dir");
	for (int i = 0; i < 60; i++) { char n[32]; snprintf(n, sizeof n, "file-%03d.dat", i); CHECK(bw_create(v, d.ref, n, BW_TYPE_FILE, 0644, NULL) == 0, "create"); }
	static struct names all; memset(&all, 0, sizeof all);
	uint64_t cookie = 0; int rounds = 0;
	stop_after = 7;
	do {
		struct names chunk; memset(&chunk, 0, sizeof chunk); uint64_t nx = 0;
		{ int rr = bw_readdir(v, d.ref, cookie, &chunk, collect_stop, &nx); CHECK(rr == 0, "readdir round %d cookie %llu: %s", rounds, (unsigned long long)cookie, strerror(-rr)); }
		for (int i = 0; i < chunk.c && all.c < 64; i++) { CHECK(!has(&all, chunk.n[i]), "duplicate %s", chunk.n[i]); strlcpy(all.n[all.c++], chunk.n[i], 256); }
		cookie = nx; rounds++;
	} while (cookie && rounds < 50);
	CHECK(all.c == 60 && rounds >= 9, "paged %d entries in %d rounds", all.c, rounds);
	CHECK(bw_unmount(v) == 0, "unmount"); v = NULL;
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

/* the kernel-offloaded I/O path: map extents, write the blocks directly to the device, complete */
CASE(koio)
{
	int ok = 1; char p[1024]; snprintf(p, sizeof p, "%s/koio.img", dir); clone(img, p);
	bw_io io = open_io(p, 1); bw_vol *v = mount_rw(&io, 5000); bw_attr a, b;
	unsigned char *blk = malloc(1 << 20), *rb = malloc(1 << 20);
	for (int i = 0; i < 1 << 20; i++) blk[i] = (unsigned char)(i * 7 + 3);
	CHECK(v, "mount");
	CHECK(bw_create(v, BW_ROOT_REF, "big.bin", BW_TYPE_FILE, 0644, &a) == 0, "create");
	/* sequential write of 3 MiB in 1 MiB chunks, the way the kernel would */
	for (int c = 0; c < 3; c++) {
		bw_extent ex[16]; int n = 0;
		CHECK(bw_extents(v, a.ref, (int64_t)c << 20, 1 << 20, 1, ex, 16, &n) == 0 && n >= 1, "extents for write %d", n);
		int64_t covered = 0;
		for (int i = 0; i < n; i++) {
			CHECK(!ex[i].zero_fill && ex[i].physical > 0, "write extent has blocks");
			int64_t lo = ex[i].logical > ((int64_t)c << 20) ? ex[i].logical : ((int64_t)c << 20);
			int64_t hi = ex[i].logical + ex[i].length < ((int64_t)(c + 1) << 20) ? ex[i].logical + ex[i].length : ((int64_t)(c + 1) << 20);
			if (hi <= lo) continue;
			CHECK(pwrite(g_ctx.fd, blk + (lo - ((int64_t)c << 20)), (size_t)(hi - lo), ex[i].physical + (lo - ex[i].logical)) == hi - lo, "direct write");
			covered += hi - lo;
		}
		CHECK(covered == 1 << 20, "covered %lld", (long long)covered);
		CHECK(bw_complete_write(v, a.ref, (int64_t)(c + 1) << 20) == 0, "complete");
	}
	CHECK(bw_getattr(v, a.ref, &b) == 0 && b.size == 3 << 20 && !(b.flags & BW_F_NO_KOIO), "size %llu flags %x", (unsigned long long)b.size, b.flags);
	CHECK(bw_read(v, a.ref, 2 << 20, 1 << 20, rb) == 1 << 20 && !memcmp(rb, blk, 1 << 20), "engine reads the kernel's bytes");
	/* read mapping matches */
	{ bw_extent ex[16]; int n = 0; CHECK(bw_extents(v, a.ref, 0, 3 << 20, 0, ex, 16, &n) == 0 && n >= 1 && !ex[0].zero_fill, "read extents");
	  CHECK(pread(g_ctx.fd, rb, 4096, ex[0].physical) == 4096 && !memcmp(rb, blk, 4096), "physical bytes"); }
	/* an unaligned engine write then a read mapping: the tail past the initialized size reads zero */
	CHECK(bw_create(v, BW_ROOT_REF, "tail.bin", BW_TYPE_FILE, 0644, &a) == 0, "tail create");
	CHECK(bw_truncate(v, a.ref, 64 << 10) == 0, "tail size");
	CHECK(bw_write(v, a.ref, 0, 5000, blk) == 5000, "tail write");
	{ bw_extent ex[16]; int n = 0; CHECK(bw_extents(v, a.ref, 0, 64 << 10, 0, ex, 16, &n) == 0, "tail extents");
	  int zf = 0; for (int i = 0; i < n; i++) if (ex[i].zero_fill) zf = 1;
	  CHECK(zf, "uninitialized tail is zero-fill");
	  CHECK(pread(g_ctx.fd, rb, 8192, ex[0].physical) == 8192 && !memcmp(rb, blk, 5000), "head bytes");
	  for (int i = 5000; i < 8192; i++) CHECK(rb[i] == 0, "stale byte %d after the initialized size", i); }
	/* writing beyond the initialized size zero-fills the gap */
	{ bw_extent ex[16]; int n = 0; CHECK(bw_extents(v, a.ref, 32 << 10, 4096, 1, ex, 16, &n) == 0 && n >= 1, "gap extents");
	  CHECK(pwrite(g_ctx.fd, blk, 4096, ex[0].physical + ((32 << 10) - ex[0].logical)) == 4096, "gap write");
	  CHECK(bw_complete_write(v, a.ref, 36 << 10) == 0, "gap complete");
	  CHECK(bw_read(v, a.ref, 0, 36 << 10, rb) == 36 << 10, "gap read");
	  for (int i = 5000; i < (32 << 10); i++) CHECK(rb[i] == 0, "gap byte %d", i);
	  CHECK(!memcmp(rb + (32 << 10), blk, 4096), "gap data"); }
	CHECK(bw_unmount(v) == 0, "unmount"); v = NULL; close_io();
	io = open_io(p, 0);
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "test", BW_FSCK_CHECK, &fr) == 0 && fr.errors == 0, "fsck: %s", fr.summary); }
end:
	free(blk); free(rb);
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

/* power loss: the image is copied while mounted (never unmounted); the copy replays */
CASE(crash_replay)
{
	int ok = 1; char p[1024], q[1024]; snprintf(p, sizeof p, "%s/crash.img", dir); snprintf(q, sizeof q, "%s/crash-copy.img", dir); clone(img, p);
	bw_io io = open_io(p, 1); bw_vol *v = mount_rw(&io, 0); bw_attr a;
	CHECK(v, "mount");
	for (int i = 0; i < 20; i++) { char n[32]; snprintf(n, sizeof n, "c%02d", i); CHECK(bw_create(v, BW_ROOT_REF, n, BW_TYPE_FILE, 0644, &a) == 0 && bw_write(v, a.ref, 0, (int64_t)strlen(n), n) == (int64_t)strlen(n), "op %d", i); }
	clone(p, q);   /* the state a power cut leaves */
	bw_unmount(v); v = NULL; close_io();
	io = open_io(q, 1);
	{ bw_probe_info pi; CHECK(bw_probe(&io, &pi) == 0 && pi.kind == BW_KIND_NTFS, "probe copy"); }
	{ bw_mount_opts mo = { 1, 0, 1 }; bw_mount_info mi;
	  CHECK(bw_mount(&io, "copy", &mo, &v, &mi) == 0, "mount copy");
	  printf("  copy: read_only=%d reasons=%d replayed=%llu stale=%d\n", mi.read_only, mi.reasons, (unsigned long long)mi.replayed_txns, mi.stale); }
	for (int i = 0; i < 20; i++) { char n[32], buf[32] = {0}; snprintf(n, sizeof n, "c%02d", i);
		CHECK(bw_lookup(v, BW_ROOT_REF, n, &a) == 0 && bw_read(v, a.ref, 0, 32, buf) == (int64_t)strlen(n) && !strcmp(buf, n), "committed op %d survives", i); }
	CHECK(bw_unmount(v) == 0, "unmount copy"); v = NULL; close_io();
	io = open_io(q, 0);
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "copy", BW_FSCK_CHECK, &fr) == 0 && fr.errors == 0, "fsck: %s", fr.summary); }
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) { unlink(p); unlink(q); } return ok;
}

/* fail-stop: a device that starts failing writes stops the volume read-only, never corrupts it */
CASE(failstop)
{
	int ok = 1; char p[1024]; snprintf(p, sizeof p, "%s/fail.img", dir); clone(img, p);
	bw_vol *v = NULL; bw_io io = open_io(p, 1); v = mount_rw(&io, 0); bw_attr a;
	CHECK(v, "mount");
	CHECK(bw_create(v, BW_ROOT_REF, "before", BW_TYPE_FILE, 0644, &a) == 0, "before");
	g_ctx.fail_after = g_ctx.writes + 3;
	int r = 0, i;
	for (i = 0; i < 50 && !r; i++) { char n[32]; snprintf(n, sizeof n, "during%02d", i); r = bw_create(v, BW_ROOT_REF, n, BW_TYPE_FILE, 0644, NULL); }
	CHECK(r < 0 && bw_failed(v) && bw_read_only(v), "stopped after %d ops (r=%d failed=%d)", i, r, bw_failed(v));
	CHECK(bw_create(v, BW_ROOT_REF, "after", BW_TYPE_FILE, 0644, NULL) < 0, "refuses after stop");
	/* a failed device refuses reads too (nh_dev.c fail-stop): nothing half-written is served; remount to read */
	{ int lr = bw_lookup(v, BW_ROOT_REF, "before", &a); CHECK(lr == -EIO, "reads refused after stop (%s)", strerror(-lr)); }
	CHECK(bw_unmount(v) < 0, "unmount reports the failure"); v = NULL; close_io();
	io = open_io(p, 1);
	{ bw_mount_opts mo = { 1, 0, 1 }; bw_mount_info mi; CHECK(bw_mount(&io, "after", &mo, &v, &mi) == 0, "remount"); }
	CHECK(bw_lookup(v, BW_ROOT_REF, "before", &a) == 0, "committed op survives");
	CHECK(bw_unmount(v) == 0, "unmount"); v = NULL; close_io();
	io = open_io(p, 0);
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "after", BW_FSCK_CHECK, &fr) == 0 && fr.errors == 0, "fsck: %s", fr.summary); }
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

/* a dirty volume (Windows did not shut down cleanly) mounts read-only with the reason */
CASE(dirty_ro)
{
	int ok = 1; bw_vol *v = NULL; char p[1024]; snprintf(p, sizeof p, "%s/dirty.img", dir); clone(img, p);
	/* set VOLUME_IS_DIRTY through the harness's setflag command */
	char cmd[2048]; snprintf(cmd, sizeof cmd, "'%s' setflag --image '%s' --rw %s >/dev/null 2>&1", getenv("BW_HARNESS"), p, "dirty");
	CHECK(system(cmd) == 0, "setflag");
	bw_io io = open_io(p, 1); bw_probe_info pi;
	CHECK(bw_probe(&io, &pi) == 0 && pi.dirty, "probe sees dirty");
	bw_mount_opts mo = { 1, 0, 1 }; bw_mount_info mi;
	CHECK(bw_mount(&io, "dirty", &mo, &v, &mi) == 0 && mi.read_only && (mi.reasons & BW_RO_DIRTY), "ro=%d reasons=%d", mi.read_only, mi.reasons);
	CHECK(bw_create(v, BW_ROOT_REF, "x", BW_TYPE_FILE, 0644, NULL) == -EROFS, "EROFS");
	bw_unmount(v); v = NULL;
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "dirty", BW_FSCK_QUICK, &fr) == 0 && fr.dirty, "quick fsck: %s", fr.summary); }
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

/* hibernated Windows (hiberfil.sys "hibr"): read-only with the reason */
CASE(hibernated_ro)
{
	int ok = 1; bw_vol *v = NULL; char p[1024]; snprintf(p, sizeof p, "%s/hib.img", dir); clone(img, p);
	char cmd[2048]; snprintf(cmd, sizeof cmd, "'%s' setflag --image '%s' --rw hiberfile >/dev/null 2>&1", getenv("BW_HARNESS"), p);
	CHECK(system(cmd) == 0, "setflag");
	bw_io io = open_io(p, 1);
	bw_mount_opts mo = { 1, 0, 1 }; bw_mount_info mi;
	CHECK(bw_mount(&io, "hib", &mo, &v, &mi) == 0 && mi.read_only && (mi.reasons & BW_RO_HIBERNATED), "ro=%d reasons=%d", mi.read_only, mi.reasons);
	bw_attr a; CHECK(bw_lookup(v, BW_ROOT_REF, "hiberfil.sys", &a) == 0, "readable");
	bw_unmount(v); v = NULL;
	{ bw_fsck_result fr; CHECK(bw_fsck(&io, "hib", BW_FSCK_QUICK, &fr) == 0 && fr.hibernated, "quick fsck: %s", fr.summary); }
end:
	if (v) bw_unmount(v);
	close_io(); if (ok) unlink(p); return ok;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: bw_api_test <journaled image> <scratch dir> [case]\n"); return 2; }
	const char *img = argv[1], *dir = argv[2], *only = argc > 3 ? argv[3] : NULL;
	printf("engine %s\n", bw_engine_version());
#define MAYBE(n) if (!only || !strcmp(only, #n)) RUN(n)
	MAYBE(probe); MAYBE(items); MAYBE(readdir_paging); MAYBE(koio); MAYBE(crash_replay); MAYBE(failstop); MAYBE(dirty_ro); MAYBE(hibernated_ro);
	printf("bw_api_test %d/%d %s\n", npass, ntotal, npass == ntotal ? "PASS" : "FAIL");
	return npass == ntotal ? 0 : 1;
}

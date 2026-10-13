/*
 * nh_bench.c - workloads (NTFS_PLAN §6.4) over two backends, the crash manifest,
 * and verify.
 *
 * Backends
 *   ntfs : libntfs-3g on an image through the bwio device. Handles keep the
 *          inode and the $DATA attribute open (the FSKit shape); fsync(h) is
 *          ntfs_inode_sync + device sync (F_FULLFSYNC on the image file).
 *   posix: a directory on a mounted volume (exFAT image through Apple's driver,
 *          or an APFS folder). fsync(h) is F_FULLFSYNC on the file.
 * Every workload ends with one durability barrier inside the timed region.
 *
 * Manifest (crash runs): append-only text, written with write(2) so a kill -9
 * keeps every line that was written:
 *   B <op> <kind> <path> [<path2>]           before the op
 *   D <op> <kind> <path> [<path2>] <size> <sha256|->   after the op returned 0
 *   C <op>                                   journaled: ops <= op are durable (the commit returned 0)
 *   T <op> <B|D|C> <unix_s.us>               wall-clock time of the line before it
 * A failed op gets no D and no C line: it stays uncertain (B only). When the device
 * fails (fail-stop, e.g. the stick was pulled) the workload stops after that op.
 * In power-loss runs a marker with <op> goes into the device write log after
 * each D line, and crashimg reports the last op whose writes all survived.
 */
#include "nh_internal.h"
#include <dirent.h>

typedef struct nh_be nh_be;
typedef struct { ntfs_inode *ni; ntfs_attr *na; int fd; int wrote; } nh_h;
struct nh_be {
	int is_ntfs;
	ntfs_volume *vol;
	char root[NH_PATHMAX];
	int nocache;
};

static void bpath(nh_be *b, const char *p, char *out) { snprintf(out, NH_PATHMAX, "%s%s", b->root, p); }

static int be_mkdir(nh_be *b, const char *p)
{
	if (b->is_ntfs) {
		ntfs_inode *ni = nh_lookup(b->vol, p);
		if (ni) { ntfs_inode_close(ni); return 0; }
		return nh_mkdir(b->vol, p);
	}
	char fp[NH_PATHMAX]; bpath(b, p, fp);
	return (mkdir(fp, 0755) && errno != EEXIST) ? -errno : 0;
}
static int be_open(nh_be *b, const char *p, int create, int trunc, nh_h *h)
{
	memset(h, 0, sizeof *h); h->fd = -1;
	if (b->is_ntfs) {
		int r = nh_open_data(b->vol, p, NULL, create, &h->ni, &h->na);
		if (r) return r;
		if (trunc && h->na->data_size) { if (ntfs_attr_truncate(h->na, 0)) { r = -errno; nh_close_data(h->ni, h->na, 0); return r; } h->wrote = 1; }
		return 0;
	}
	char fp[NH_PATHMAX]; bpath(b, p, fp);
	h->fd = open(fp, O_RDWR | (create ? O_CREAT : 0) | (trunc ? O_TRUNC : 0), 0644);
	if (h->fd < 0) return -errno;
	if (b->nocache) fcntl(h->fd, F_NOCACHE, 1);
	return 0;
}
static s64 be_pwrite(nh_be *b, nh_h *h, s64 off, s64 len, const void *buf)
{
	h->wrote = 1;
	if (b->is_ntfs) return nh_pwrite_all(h->na, off, len, buf);
	s64 d = 0; while (d < len) { ssize_t w = pwrite(h->fd, (const char *)buf + d, len - d, off + d); if (w <= 0) return -1; d += w; }
	return d;
}
static s64 be_pread(nh_be *b, nh_h *h, s64 off, s64 len, void *buf)
{
	if (b->is_ntfs) return nh_pread_all(h->na, off, len, buf);
	s64 d = 0; while (d < len) { ssize_t r = pread(h->fd, (char *)buf + d, len - d, off + d); if (r < 0) return -1; if (!r) break; d += r; }
	return d;
}
static int be_truncate(nh_be *b, nh_h *h, s64 sz)
{
	h->wrote = 1;
	if (b->is_ntfs) return ntfs_attr_truncate(h->na, sz) ? -errno : 0;
	return ftruncate(h->fd, sz) ? -errno : 0;
}
static s64 be_size(nh_be *b, nh_h *h)
{
	if (b->is_ntfs) return h->na->data_size;
	struct stat st; fstat(h->fd, &st); return st.st_size;
}
static int be_fsync(nh_be *b, nh_h *h)
{
	if (b->is_ntfs) { if (ntfs_inode_sync(h->ni)) return -errno; return nh_sync(b->vol) ? -errno : 0; }
	return fcntl(h->fd, F_FULLFSYNC, 0) == -1 ? -errno : 0;
}
static int be_close(nh_be *b, nh_h *h)
{
	if (b->is_ntfs) return nh_close_data(h->ni, h->na, h->wrote);
	return close(h->fd) ? -errno : 0;
}
static int be_rename(nh_be *b, const char *a, const char *c)
{
	if (b->is_ntfs) return nh_rename(b->vol, a, c);
	char fa[NH_PATHMAX], fc[NH_PATHMAX]; bpath(b, a, fa); bpath(b, c, fc);
	return rename(fa, fc) ? -errno : 0;
}
static int be_unlink(nh_be *b, const char *p)
{
	if (b->is_ntfs) return nh_unlink(b->vol, p);
	char fp[NH_PATHMAX]; bpath(b, p, fp);
	return unlink(fp) ? -errno : 0;
}
/* volume-wide barrier */
static int be_sync(nh_be *b)
{
	if (b->is_ntfs) return nh_sync(b->vol) ? -errno : 0;
	sync();
	int fd = open(b->root, O_RDONLY);
	if (fd >= 0) { fcntl(fd, F_FULLFSYNC, 0); close(fd); }
	return 0;
}

/* ---------------- manifest ---------------- */
static int mfd = -1;
static uint64_t opno;
static nh_be *g_be;
static int g_oprc;    /* first error inside the current op (0 = the op succeeded) */
static int g_stop;    /* the device is fail-stop: the workload loop ends */
static void mline(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void mline(const char *fmt, ...)
{
	if (mfd < 0) return;
	char buf[5 * NH_PATHMAX];
	va_list ap; va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0) write(mfd, buf, n);
}
/* manifest paths: '%', whitespace and control bytes are %XX-escaped */
void nh_menc(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (; *in && o + 4 < cap; in++) {
		unsigned char c = (unsigned char)*in;
		if (c <= ' ' || c == '%' || c == 0x7f) o += snprintf(out + o, cap - o, "%%%02X", c);
		else out[o++] = c;
	}
	out[o] = 0;
}
void nh_mdec(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '%' && r[1] && r[2]) { char h[3] = { r[1], r[2], 0 }; *w++ = (char)strtol(h, NULL, 16); r += 2; }
		else *w++ = *r;
	}
	*w = 0;
}
/* "T <op> <B|D|C> <unix seconds.microseconds>" after each B, D and C line: a separate line, because
 * verify reads a B line's optional fourth field as a second path. Readers skip unknown tags. */
static void mtime(char tag)
{
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	mline("T %llu %c %lld.%06ld\n", (unsigned long long)opno, tag, (long long)ts.tv_sec, ts.tv_nsec / 1000);
}
static void op_begin(const char *kind, const char *a, const char *b2)
{
	char ea[2 * NH_PATHMAX], eb[2 * NH_PATHMAX] = "";
	nh_menc(a, ea, sizeof ea); if (b2) nh_menc(b2, eb, sizeof eb);
	opno++; g_oprc = 0;
	mline("B %llu %s %s%s%s\n", (unsigned long long)opno, kind, ea, b2 ? " " : "", eb);
	mtime('B');
}
static void op_done(const char *kind, const char *a, const char *b2, s64 size, const char *sha)
{
	char ea[2 * NH_PATHMAX], eb[2 * NH_PATHMAX] = "";
	nh_menc(a, ea, sizeof ea); if (b2) nh_menc(b2, eb, sizeof eb);
	nh_bwdev *dev = g_be && g_be->is_ntfs ? nh_dev_of(g_be->vol) : NULL;
	if (g_oprc) {
		/* the op failed: no D (it did not return 0) and no C (nothing new may be claimed durable) */
		fprintf(stderr, "op %llu: failed (%s); no D/C recorded\n", (unsigned long long)opno, nh_errstr(g_oprc));
		if (dev) nh_op_failed(g_be->vol, g_oprc);   /* group commit: fail-stop */
	} else {
		mline("D %llu %s %s%s%s %lld %s\n", (unsigned long long)opno, kind, ea, b2 ? " " : "", eb, (long long)size, sha ? sha : "-");
		mtime('D');
		/* journaled: the commit point. "C n" = ops <= n are durable (written only after the commit returned 0) */
		if (dev && nh_wlog_marker(g_be->vol, opno)) { mline("C %llu\n", (unsigned long long)opno); mtime('C'); }
	}
	if (dev && dev->failed && !g_stop) {
		g_stop = 1;
		fprintf(stderr, "bench: device failed (%s) at op %llu; stopping\n", strerror(dev->failed), (unsigned long long)opno);
	}
}

void nh_mopen(int fd) { mfd = fd; opno = 0; g_be = NULL; g_oprc = 0; g_stop = 0; }
void nh_mcreate(const char *path, s64 size, const char *sha) { op_begin("create", path, NULL); op_done("create", path, NULL, size, sha); }
static uint64_t path_seed(const char *p) { uint64_t h = 1469598103934665603ULL; for (; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; } return h; }

/* write a whole file of generated content; returns sha in hex */
static int write_file(nh_be *b, const char *p, s64 size, s64 chunk, uint64_t seed, char hex[65], int do_sha, int fsync_it)
{
	nh_h h; int r = be_open(b, p, 1, 1, &h);
	if (r) return r;
	static unsigned char *buf; static s64 bcap;
	if (chunk > bcap) { free(buf); buf = malloc(chunk); bcap = chunk; }
	CC_SHA256_CTX c; if (do_sha) CC_SHA256_Init(&c);
	for (s64 off = 0; off < size; off += chunk) {
		s64 n = size - off < chunk ? size - off : chunk;
		nh_fill(buf, n, seed, off);
		if (be_pwrite(b, &h, off, n, buf) != n) { r = -EIO; break; }
		if (do_sha) CC_SHA256_Update(&c, buf, (CC_LONG)n);
	}
	if (!r && fsync_it) r = be_fsync(b, &h);
	int r2 = be_close(b, &h); if (!r) r = r2;
	if (do_sha) { unsigned char md[32]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex); }
	return r;
}

typedef struct { s64 size, chunk, count; int sync_every; uint64_t seed; const char *dir; } wparams;
typedef struct { double secs, sync_secs; s64 bytes; s64 ops; int errors; } wresult;

#define CHECK(x) do { int _r = (x); if (_r) { if (!g_oprc) g_oprc = _r; res->errors++; if (res->errors < 5) fprintf(stderr, "op %llu: %s: %s\n", (unsigned long long)opno, #x, nh_errstr(_r)); } } while (0)

static void w_seqwrite(nh_be *b, wparams *p, wresult *res)
{
	char path[NH_PATHMAX], hex[65] = "-"; snprintf(path, sizeof path, "%s/seq.bin", p->dir);
	op_begin("create", path, NULL);
	CHECK(write_file(b, path, p->size, p->chunk, path_seed(path), hex, mfd >= 0, 1));
	op_done("create", path, NULL, p->size, hex);
	res->bytes = p->size; res->ops = p->size / p->chunk;
}
static void w_seqread(nh_be *b, wparams *p, wresult *res)
{
	char path[NH_PATHMAX]; snprintf(path, sizeof path, "%s/seq.bin", p->dir);
	nh_h h; int r = be_open(b, path, 0, 0, &h);
	if (r) { CHECK(r); return; }
	s64 size = be_size(b, &h);
	unsigned char *buf = malloc(p->chunk);
	for (s64 off = 0; off < size; off += p->chunk) {
		s64 n = size - off < p->chunk ? size - off : p->chunk;
		if (be_pread(b, &h, off, n, buf) != n) { res->errors++; break; }
		res->bytes += n; res->ops++;
	}
	be_close(b, &h); free(buf);
}
static void w_rand4k(nh_be *b, wparams *p, wresult *res, int write)
{
	char path[NH_PATHMAX]; snprintf(path, sizeof path, "%s/seq.bin", p->dir);
	nh_h h; int r = be_open(b, path, 0, 0, &h);
	if (r) { CHECK(r); return; }
	s64 blocks = be_size(b, &h) / 4096;
	unsigned char buf[4096]; uint64_t s = p->seed;
	for (s64 i = 0; i < p->count; i++) {
		s64 off = (s64)(nh_rand(&s) % blocks) * 4096;
		if (write) {
			/* keep content regenerable: rewrite the same generated bytes */
			nh_fill(buf, 4096, path_seed(path), off);
			if (be_pwrite(b, &h, off, 4096, buf) != 4096) res->errors++;
		} else if (be_pread(b, &h, off, 4096, buf) != 4096) res->errors++;
		res->ops++; res->bytes += 4096;
	}
	if (write) CHECK(be_fsync(b, &h));
	be_close(b, &h);
}
static void w_smallcreate(nh_be *b, wparams *p, wresult *res, s64 fsize, const char *sub)
{
	char d[NH_PATHMAX], path[NH_PATHMAX], hex[65] = "-";
	snprintf(d, sizeof d, "%s/%s", p->dir, sub);
	CHECK(be_mkdir(b, d));
	for (s64 i = 0; i < p->count; i++) {
		snprintf(path, sizeof path, "%s/f%06lld", d, (long long)i);
		op_begin("create", path, NULL);
		strcpy(hex, "-");
		CHECK(write_file(b, path, fsize, fsize < (1 << 20) ? fsize : (1 << 20), path_seed(path), hex, mfd >= 0, 0));
		op_done("create", path, NULL, fsize, hex);
		if (g_stop) break;
		res->ops++; res->bytes += fsize;
		if (p->sync_every && (i + 1) % p->sync_every == 0) CHECK(be_sync(b));
	}
}
static void w_rename(nh_be *b, wparams *p, wresult *res)
{
	char a[NH_PATHMAX], c[NH_PATHMAX];
	for (s64 i = 0; i < p->count; i++) {
		snprintf(a, sizeof a, "%s/small/f%06lld", p->dir, (long long)i);
		snprintf(c, sizeof c, "%s/small/r%06lld", p->dir, (long long)i);
		op_begin("rename", a, c);
		CHECK(be_rename(b, a, c));
		op_done("rename", a, c, 0, NULL);
		if (g_stop) break;
		res->ops++;
		if (p->sync_every && (i + 1) % p->sync_every == 0) CHECK(be_sync(b));
	}
}
static void w_delete(nh_be *b, wparams *p, wresult *res)
{
	char a[NH_PATHMAX];
	for (s64 i = 0; i < p->count; i++) {
		char f[NH_PATHMAX];
		snprintf(a, sizeof a, "%s/small/r%06lld", p->dir, (long long)i);
		snprintf(f, sizeof f, "%s/small/f%06lld", p->dir, (long long)i);
		op_begin("rm", a, f);   /* either name may be the one removed: both uncertain while in flight */
		int r = be_unlink(b, a);
		if (r == -ENOENT) { snprintf(a, sizeof a, "%s/small/f%06lld", p->dir, (long long)i); r = be_unlink(b, a); }
		CHECK(r);
		op_done("rm", a, NULL, 0, NULL);
		if (g_stop) break;
		res->ops++;
		if (p->sync_every && (i + 1) % p->sync_every == 0) CHECK(be_sync(b));
	}
}
/* truncate/extend: a model of the file is kept in memory so every state has a sha */
static void w_truncate(nh_be *b, wparams *p, wresult *res)
{
	char path[NH_PATHMAX]; snprintf(path, sizeof path, "%s/trunc.bin", p->dir);
	const s64 MAX = 16 << 20;
	unsigned char *model = calloc(1, MAX), *chunk = malloc(65536);
	s64 cur = 0; uint64_t s = p->seed;
	nh_h h; int r = be_open(b, path, 1, 1, &h); if (r) { CHECK(r); return; }
	for (s64 i = 0; i < p->count; i++) {
		s64 ns = (s64)(nh_rand(&s) % (MAX - 65536));
		op_begin("truncate", path, NULL);
		CHECK(be_truncate(b, &h, ns));
		if (ns > cur) memset(model + cur, 0, ns - cur);
		cur = ns;
		if (i % 4 == 0) { /* write 64 KiB at the end: extend with data */
			nh_fill(chunk, 65536, s, cur);
			if (be_pwrite(b, &h, cur, 65536, chunk) != 65536) { res->errors++; if (!g_oprc) g_oprc = -EIO; }
			memcpy(model + cur, chunk, 65536); cur += 65536;
		}
		char hex[65] = "-";
		if (mfd >= 0) { CHECK(be_fsync(b, &h)); unsigned char md[32]; CC_SHA256(model, (CC_LONG)cur, md); nh_sha_hex(md, hex); }
		op_done("truncate", path, NULL, cur, hex);
		if (g_stop) break;
		res->ops++;
	}
	CHECK(be_fsync(b, &h));
	be_close(b, &h); free(model); free(chunk);
}
/* append-heavy log: records of 256 bytes, fsync every sync_every records */
static void w_append(nh_be *b, wparams *p, wresult *res)
{
	char path[NH_PATHMAX]; snprintf(path, sizeof path, "%s/app.log", p->dir);
	nh_h h; int r = be_open(b, path, 1, 1, &h); if (r) { CHECK(r); return; }
	CC_SHA256_CTX c; CC_SHA256_Init(&c);
	unsigned char rec[256]; s64 off = 0;
	int se = p->sync_every ? p->sync_every : 1000;
	op_begin("append", path, NULL);
	for (s64 i = 0; i < p->count; i++) {
		nh_fill(rec, sizeof rec, path_seed(path), off);
		if (be_pwrite(b, &h, off, sizeof rec, rec) != (s64)sizeof rec) { res->errors++; break; }
		CC_SHA256_Update(&c, rec, sizeof rec);
		off += sizeof rec; res->ops++; res->bytes += sizeof rec;
		if ((i + 1) % se == 0 || i + 1 == p->count) {
			CHECK(be_fsync(b, &h));
			char hex[65] = "-";
			if (mfd >= 0) { CC_SHA256_CTX c2 = c; unsigned char md[32]; CC_SHA256_Final(md, &c2); nh_sha_hex(md, hex); }
			op_done("append", path, NULL, off, hex);
			if (g_stop) break;
			if (i + 1 < p->count) op_begin("append", path, NULL);
		}
	}
	be_close(b, &h);
}
/* mixed: create / overwrite / read / rename / delete / append over 8 dirs */
static void w_mixed(nh_be *b, wparams *p, wresult *res)
{
	const int NDIR = 8, POOL = 4096;
	char (*live)[256] = calloc(POOL, 256);
	int nlive = 0; uint64_t s = p->seed; s64 nextid = 0;
	char d[NH_PATHMAX], path[NH_PATHMAX], path2[NH_PATHMAX], hex[65];
	for (int i = 0; i < NDIR; i++) { snprintf(d, sizeof d, "%s/mix%d", p->dir, i); CHECK(be_mkdir(b, d)); }
	unsigned char *buf = malloc(1 << 20);
	for (s64 i = 0; i < p->count; i++) {
		int k = (int)(nh_rand(&s) % 100);
		if (nlive < 32 || (k < 35 && nlive < POOL)) {
			snprintf(path, sizeof path, "%s/mix%d/m%07lld", p->dir, (int)(nh_rand(&s) % NDIR), (long long)nextid++);
			s64 sz = 1 + (s64)(nh_rand(&s) % 65536);
			if (nh_rand(&s) % 16 == 0) sz = (s64)(nh_rand(&s) % (1 << 20));
			op_begin("create", path, NULL);
			strcpy(hex, "-");
			CHECK(write_file(b, path, sz, 1 << 20, path_seed(path) ^ (uint64_t)i, hex, mfd >= 0, 0));
			op_done("create", path, NULL, sz, mfd >= 0 ? hex : NULL);
			strlcpy(live[nlive++], path, 256);
			res->bytes += sz;
		} else if (k < 55) { /* read whole */
			int j = (int)(nh_rand(&s) % nlive);
			nh_h h; if (!be_open(b, live[j], 0, 0, &h)) { s64 sz = be_size(b, &h); for (s64 o = 0; o < sz; o += 1 << 20) { s64 n = sz - o < (1 << 20) ? sz - o : (1 << 20); if (be_pread(b, &h, o, n, buf) != n) res->errors++; res->bytes += n; } be_close(b, &h); } else res->errors++;
		} else if (k < 70) { /* rewrite with new content (new seed) */
			int j = (int)(nh_rand(&s) % nlive);
			s64 sz = 1 + (s64)(nh_rand(&s) % 65536);
			op_begin("create", live[j], NULL);
			strcpy(hex, "-");
			CHECK(write_file(b, live[j], sz, 1 << 20, path_seed(live[j]) ^ (uint64_t)i, hex, mfd >= 0, 0));
			op_done("create", live[j], NULL, sz, mfd >= 0 ? hex : NULL);
			res->bytes += sz;
		} else if (k < 85) { /* rename, possibly across dirs */
			int j = (int)(nh_rand(&s) % nlive);
			snprintf(path2, sizeof path2, "%s/mix%d/n%07lld", p->dir, (int)(nh_rand(&s) % NDIR), (long long)nextid++);
			op_begin("rename", live[j], path2);
			CHECK(be_rename(b, live[j], path2));
			op_done("rename", live[j], path2, 0, NULL);
			strlcpy(live[j], path2, 256);
		} else { /* delete */
			int j = (int)(nh_rand(&s) % nlive);
			op_begin("rm", live[j], NULL);
			CHECK(be_unlink(b, live[j]));
			op_done("rm", live[j], NULL, 0, NULL);
			strlcpy(live[j], live[--nlive], 256);
		}
		if (g_stop) break;
		res->ops++;
		if (p->sync_every && (i + 1) % p->sync_every == 0) CHECK(be_sync(b));
	}
	free(buf); free(live);
}

static const char *val(int argc, char **argv, const char *k) { for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], k)) return argv[i + 1]; return NULL; }
static s64 sval(int argc, char **argv, const char *k, s64 def)
{
	const char *v = val(argc, argv, k); if (!v) return def;
	char *e; double x = strtod(v, &e);
	if (*e == 'K' || *e == 'k') x *= 1024; else if (*e == 'M' || *e == 'm') x *= 1 << 20; else if (*e == 'G' || *e == 'g') x *= 1 << 30;
	return (s64)x;
}

int cmd_bench(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *wl = val(argc, argv, "--workload"), *posix = val(argc, argv, "--posix"), *man = val(argc, argv, "--manifest");
	if (!wl) { fprintf(stderr, "bench: --workload seqwrite|seqread|randread|randwrite|small|medium|rename|delete|truncate|append|mixed\n"); return 2; }
	wparams p = { sval(argc, argv, "--size", 4LL << 30), sval(argc, argv, "--chunk", 1 << 20), sval(argc, argv, "--count", 10000),
		      (int)sval(argc, argv, "--sync-every", 0), (uint64_t)sval(argc, argv, "--seed", 42), val(argc, argv, "--dir") };
	if (!p.dir) p.dir = "/w";
	nh_be be = { 0 };
	be.nocache = o.nocache;
	if (posix) {
		if (!nh_path_allowed(posix)) { fprintf(stderr, "refused: %s outside %s\n", posix, NH_ALLOWED_ROOT); return 1; }
		strlcpy(be.root, posix, sizeof be.root);
	} else {
		be.is_ntfs = 1;
		int err; be.vol = nh_mount(&o, 0, &err);
		if (!be.vol) { fprintf(stderr, "mount failed: %s\n", strerror(err)); return 3; }
		if (NVolReadOnly(be.vol)) { fprintf(stderr, "refused: volume mounted read-only by the guard (EROFS)\n"); nh_umount(be.vol); return 30; }
	}
	g_be = &be;
	if (man) {
		if (!nh_path_allowed(man)) { fprintf(stderr, "refused manifest path\n"); return 1; }
		mfd = open(man, O_WRONLY | O_CREAT | O_APPEND, 0644);
		const char *start = val(argc, argv, "--op-start"); opno = start ? strtoull(start, NULL, 0) : 0;
	}
	/* make the workload directory chain */
	if (strcmp(p.dir, "/")) { char tmp[NH_PATHMAX]; strlcpy(tmp, p.dir, sizeof tmp); for (char *q = tmp + 1; ; q++) { if (*q == '/' || !*q) { char c = *q; *q = 0; be_mkdir(&be, tmp); *q = c; if (!c) break; } } }
	wresult res = { 0 };
	nh_bwdev *dev = be.is_ntfs ? &nh_last_stats : NULL;
	double t = nh_now();
	if (!strcmp(wl, "seqwrite")) w_seqwrite(&be, &p, &res);
	else if (!strcmp(wl, "seqread")) w_seqread(&be, &p, &res);
	else if (!strcmp(wl, "randread")) w_rand4k(&be, &p, &res, 0);
	else if (!strcmp(wl, "randwrite")) w_rand4k(&be, &p, &res, 1);
	else if (!strcmp(wl, "small")) w_smallcreate(&be, &p, &res, sval(argc, argv, "--fsize", 4096), "small");
	else if (!strcmp(wl, "medium")) w_smallcreate(&be, &p, &res, sval(argc, argv, "--fsize", 1 << 20), "medium");
	else if (!strcmp(wl, "rename")) w_rename(&be, &p, &res);
	else if (!strcmp(wl, "delete")) w_delete(&be, &p, &res);
	else if (!strcmp(wl, "truncate")) w_truncate(&be, &p, &res);
	else if (!strcmp(wl, "append")) w_append(&be, &p, &res);
	else if (!strcmp(wl, "mixed")) w_mixed(&be, &p, &res);
	else { fprintf(stderr, "unknown workload %s\n", wl); return 2; }
	if (be_sync(&be)) res.errors++;
	double t_sync = nh_now();
	/* the unmount is the last flush of the library's caches: it is inside the timed region */
	int ur = be.is_ntfs ? nh_umount(be.vol) : 0;
	res.secs = nh_now() - t;
	double umount_s = nh_now() - t_sync;
	if (ur) res.errors++;
	printf("bench backend=%s workload=%s chunk=%lld count=%lld size=%lld ops=%lld bytes=%lld secs=%.3f MBps=%.1f opsps=%.0f errors=%d",
	       be.is_ntfs ? "ntfs" : "posix", wl, (long long)p.chunk, (long long)p.count, (long long)p.size, (long long)res.ops, (long long)res.bytes,
	       res.secs, res.bytes / res.secs / 1048576.0, res.ops / res.secs, res.errors);
	if (dev) printf(" dev_writes=%llu dev_wbytes=%llu dev_reads=%llu dev_rbytes=%llu syncs=%llu sync_secs=%.3f umount_secs=%.3f",
			(unsigned long long)dev->nwrites, (unsigned long long)dev->bytes_written, (unsigned long long)dev->nreads,
			(unsigned long long)dev->bytes_read, (unsigned long long)dev->nsyncs, dev->sync_secs, umount_s);
	if (dev && dev->wal_commits) printf(" wal_commits=%llu wal_txn_blocks=%llu wal_jbytes=%llu wal_direct_bytes=%llu wal_inplace_bytes=%llu wal_meta_logical=%llu wal_checkpoints=%llu wal_forced=%llu wal_lib_syncs=%llu wal_b1_skipped=%llu",
			(unsigned long long)dev->wal_commits, (unsigned long long)dev->wal_txn_blocks, (unsigned long long)dev->wal_jbytes,
			(unsigned long long)dev->wal_direct_bytes, (unsigned long long)dev->wal_inplace_bytes, (unsigned long long)dev->wal_logical_meta_bytes,
			(unsigned long long)dev->wal_checkpoints, (unsigned long long)dev->wal_forced_commits, (unsigned long long)dev->wal_lib_syncs, (unsigned long long)dev->wal_b1_skipped);
	if (dev && dev->failed) printf(" dev_failed=%s", strerror(dev->failed));
	printf(" last_op=%llu\n", (unsigned long long)opno);
	if (mfd >= 0) close(mfd);
	return res.errors ? 5 : 0;
}

/* ---------------- verify ---------------- */
typedef struct { char *path; s64 size; char sha[65]; int live; } ment;
typedef struct { ment *v; size_t n, cap; } mstate;
static ment *mfind(mstate *m, const char *p) { for (size_t i = m->n; i-- > 0;) if (m->v[i].live && !strcmp(m->v[i].path, p)) return &m->v[i]; return NULL; }
static void mput(mstate *m, const char *p, s64 sz, const char *sha)
{
	ment *e = mfind(m, p);
	if (!e) { if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 1024; m->v = realloc(m->v, m->cap * sizeof *m->v); } e = &m->v[m->n++]; e->path = strdup(p); e->live = 1; }
	e->size = sz; strlcpy(e->sha, sha, sizeof e->sha);
}
/* uncertain set: simple string list */
typedef struct { char **v; size_t n, cap; } slist;
static void sadd(slist *s, const char *p) { if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = realloc(s->v, s->cap * sizeof *s->v); } s->v[s->n++] = strdup(p); }
static int shas(slist *s, const char *p) { for (size_t i = 0; i < s->n; i++) if (!strcmp(s->v[i], p)) return 1; return 0; }

struct walkctx { nh_be *be; mstate *m; slist *unc; int errors, checked, uncertain_seen, unexpected; };
static void check_file(struct walkctx *w, const char *p)
{
	if (shas(w->unc, p)) { w->uncertain_seen++; return; }
	ment *e = mfind(w->m, p);
	if (!e) { w->unexpected++; w->errors++; if (w->errors < 20) printf("ERROR unexpected file %s\n", p); return; }
	e->live = 2; /* seen */
	if (!strcmp(e->sha, "-")) { w->checked++; return; }
	char hex[65]; s64 sz = 0; int r;
	if (w->be->is_ntfs) r = nh_sha_stream(w->be->vol, p, NULL, hex, &sz);
	else {
		char fp[NH_PATHMAX]; bpath(w->be, p, fp);
		int fd = open(fp, O_RDONLY); r = fd < 0 ? -errno : 0;
		if (!r) { CC_SHA256_CTX c; CC_SHA256_Init(&c); static unsigned char buf[1 << 20]; ssize_t n; while ((n = read(fd, buf, sizeof buf)) > 0) { CC_SHA256_Update(&c, buf, (CC_LONG)n); sz += n; } close(fd); unsigned char md[32]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex); }
	}
	w->checked++;
	if (r) { w->errors++; printf("ERROR %s: read failed %s\n", p, nh_errstr(r)); return; }
	if (sz != e->size || strcmp(hex, e->sha)) { w->errors++; if (w->errors < 20) printf("ERROR %s: content mismatch size %lld/%lld sha %.12s/%.12s\n", p, (long long)sz, (long long)e->size, hex, e->sha); }
}
struct nlist { char **v; int *isdir; size_t n, cap; };
static int vcb(void *d, const ntfschar *name, const int len, const int type, const s64 pos, const MFT_REF mref, const unsigned dt)
{
	struct nlist *l = d; (void)pos; (void)mref;
	if (type == FILE_NAME_DOS) return 0;
	char *s = NULL; if (ntfs_ucstombs(name, len, &s, 0) < 0) return 0;
	if (!strcmp(s, ".") || !strcmp(s, "..")) { free(s); return 0; }
	if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = realloc(l->v, l->cap * sizeof *l->v); l->isdir = realloc(l->isdir, l->cap * sizeof(int)); }
	l->v[l->n] = s; l->isdir[l->n] = dt == NTFS_DT_DIR; l->n++;
	return 0;
}
static void walk(struct walkctx *w, const char *dir)
{
	struct nlist l = { 0 };
	if (w->be->is_ntfs) {
		ntfs_inode *ni = nh_lookup(w->be->vol, dir);
		if (!ni) { w->errors++; printf("ERROR cannot open dir %s\n", dir); return; }
		s64 pos = 0; if (ntfs_readdir(ni, &pos, &l, vcb)) { w->errors++; printf("ERROR readdir %s\n", dir); }
		ntfs_inode_close(ni);
	} else {
		char fp[NH_PATHMAX]; bpath(w->be, dir, fp);
		DIR *d = opendir(fp); if (!d) { w->errors++; printf("ERROR opendir %s\n", fp); return; }
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, "._", 2) || !strcmp(e->d_name, ".DS_Store") || !strcmp(e->d_name, ".fseventsd") || !strncmp(e->d_name, ".Spotlight", 10) || !strncmp(e->d_name, ".Trashes", 8)) continue;
			if (l.n == l.cap) { l.cap = l.cap ? l.cap * 2 : 256; l.v = realloc(l.v, l.cap * sizeof *l.v); l.isdir = realloc(l.isdir, l.cap * sizeof(int)); }
			l.v[l.n] = strdup(e->d_name); l.isdir[l.n] = e->d_type == DT_DIR; l.n++;
		}
		closedir(d);
	}
	for (size_t i = 0; i < l.n; i++) {
		char p[NH_PATHMAX]; snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", l.v[i]);
		if (l.isdir[i]) walk(w, p); else check_file(w, p);
		free(l.v[i]);
	}
	free(l.v); free(l.isdir);
}

int cmd_verify(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *man = val(argc, argv, "--manifest"), *posix = val(argc, argv, "--posix"), *root = val(argc, argv, "--root");
	const char *co = val(argc, argv, "--completed-op");
	long long completed = co ? atoll(co) : -1; /* -1: every D line counts */
	if (!man) return 2;
	if (!root) root = "/w";
	FILE *f = fopen(man, "r"); if (!f) { perror(man); return 1; }
	mstate m = { 0 }; slist unc = { 0 };
	char line[5 * NH_PATHMAX]; long long lastB = -1, lastD = -1;
	/* first pass: ops that began and ended */
	char kind[32], a[NH_PATHMAX], b2[NH_PATHMAX], sha[80]; long long n; long long sz;
	/* pending ops: B without matching D, or D beyond the completed cut */
	while (fgets(line, sizeof line, f)) {
		char t = line[0];
		if (t == 'B') {
			int k = sscanf(line, "B %lld %31s %s %s", &n, kind, a, b2);
			lastB = n; nh_mdec(a); if (k == 4) nh_mdec(b2);
			sadd(&unc, a); if (k == 4) sadd(&unc, b2); /* provisional; removed on D */
		} else if (t == 'D') {
			char kk[32] = ""; sscanf(line, "D %*lld %31s", kk);
			int two = !strcmp(kk, "rename");
			int k;
			if (two) k = sscanf(line, "D %lld %31s %s %s %lld %79s", &n, kind, a, b2, &sz, sha);
			else k = sscanf(line, "D %lld %31s %s %lld %79s", &n, kind, a, &sz, sha);
			(void)k; nh_mdec(a); if (two) nh_mdec(b2);
			if (completed >= 0 && n > completed) continue; /* stays uncertain */
			lastD = n;
			/* drop the provisional uncertainty of this op */
			for (size_t i = unc.n; i-- > 0;) if (!strcmp(unc.v[i], a)) { free(unc.v[i]); unc.v[i] = unc.v[--unc.n]; break; }
			if (two) for (size_t i = unc.n; i-- > 0;) if (!strcmp(unc.v[i], b2)) { free(unc.v[i]); unc.v[i] = unc.v[--unc.n]; break; }
			if (!strcmp(kind, "create") || !strcmp(kind, "append") || !strcmp(kind, "truncate")) mput(&m, a, sz, sha);
			else if (!strcmp(kind, "rename")) { ment *e = mfind(&m, a); if (e) { s64 s2 = e->size; char h2[65]; strlcpy(h2, e->sha, sizeof h2); e->live = 0; mput(&m, b2, s2, h2); } }
			else if (!strcmp(kind, "rm")) { ment *e = mfind(&m, a); if (e) e->live = 0; }
		}
	}
	fclose(f);
	nh_be be = { 0 };
	if (posix) { strlcpy(be.root, posix, sizeof be.root); }
	else {
		be.is_ntfs = 1; o.rw = 0;
		int err; be.vol = nh_mount(&o, 0, &err);
		if (!be.vol) { printf("verify result=FAIL mount: %s\n", strerror(err)); return 4; }
	}
	/* an uncertain path also covers an append/truncate that had an earlier completed state */
	struct walkctx w = { &be, &m, &unc, 0, 0, 0, 0 };
	int root_absent_ok = 0;
	if (be.is_ntfs && lastD < 0) {
		/* no op completed: the run's own mkdir of the root may not be durable yet (journal) */
		ntfs_inode *rni = nh_lookup(be.vol, root);
		if (rni) ntfs_inode_close(rni); else { root_absent_ok = 1; printf("note root %s absent, no op completed: nothing expected\n", root); }
	}
	if (!root_absent_ok) walk(&w, root);
	size_t expected = 0, missing = 0;
	for (size_t i = 0; i < m.n; i++) {
		if (!m.v[i].live) continue;
		expected++;
		if (m.v[i].live == 1 && !shas(&unc, m.v[i].path)) { missing++; w.errors++; if (missing < 20) printf("ERROR missing %s\n", m.v[i].path); }
	}
	if (be.is_ntfs) nh_umount(be.vol);
	printf("verify result=%s errors=%d expected=%zu checked=%d missing=%zu unexpected=%d uncertain_paths=%zu uncertain_seen=%d last_B=%lld last_D_counted=%lld\n",
	       w.errors ? "FAIL" : "PASS", w.errors, expected, w.checked, missing, w.unexpected, unc.n, w.uncertain_seen, lastB, lastD);
	return w.errors ? 4 : 0;
}

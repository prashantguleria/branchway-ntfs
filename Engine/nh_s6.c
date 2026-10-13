/*
 * nh_s6.c - stage S6 (real USB device) measurements.
 *
 *   flushbench: raw write + flush latency on an image file or the allowed device,
 *               for none / fsync / F_FULLFSYNC / DKIOCSYNCHRONIZECACHE / the harness barrier.
 *               A bridge that ignores cache flushes shows no gap between "none" and the flushes.
 *   commitlat:  latency of one journal commit (nh_commit) for small transactions,
 *               with two barriers or the adaptive one-barrier commit.
 */
#include "nh_internal.h"
#include <sys/disk.h>

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, size_t n, double p) { if (!n) return 0; size_t i = (size_t)(p * (n - 1) + 0.5); return v[i]; }
static void summary(const char *tag, double *v, size_t n)
{
	double sum = 0; for (size_t i = 0; i < n; i++) sum += v[i];
	qsort(v, n, sizeof *v, dcmp);
	printf("%s n=%zu median_ms=%.3f p90_ms=%.3f p99_ms=%.3f max_ms=%.3f mean_ms=%.3f min_ms=%.3f\n", tag, n,
	       pct(v, n, .5) * 1e3, pct(v, n, .9) * 1e3, pct(v, n, .99) * 1e3, v[n - 1] * 1e3, sum / n * 1e3, v[0] * 1e3);
}

int cmd_flushbench(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *mode = "barrier", *out = NULL; long count = 1000, bs = 4096, burst = 1, region_mib = 64, off_mib = 0;
	for (int i = 1; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--mode")) mode = argv[++i];
		else if (!strcmp(argv[i], "--count")) count = atol(argv[++i]);
		else if (!strcmp(argv[i], "--bs")) bs = atol(argv[++i]);
		else if (!strcmp(argv[i], "--burst")) burst = atol(argv[++i]);
		else if (!strcmp(argv[i], "--region-mib")) region_mib = atol(argv[++i]);
		else if (!strcmp(argv[i], "--offset-mib")) off_mib = atol(argv[++i]);
		else if (!strcmp(argv[i], "--out")) out = argv[++i];
	}
	if (!o.image || count < 1 || bs < 512 || bs % 512 || burst < 1 || region_mib < 1) { fprintf(stderr, "usage: flushbench --image T --mode none|fsync|fullfsync|dkioc|barrier [--count N] [--bs B] [--burst K] [--region-mib M] [--offset-mib O] [--out CSV]\n"); return 2; }
	int isdev = nh_dev_allowed(o.image);
	if (!isdev && access(o.image, F_OK)) { int fd = open(o.image, O_RDWR | O_CREAT, 0644); if (fd >= 0) { ftruncate(fd, (off_mib + region_mib) << 20); close(fd); } }
	int fd = nh_open_target(o.image, O_RDWR | (isdev ? O_EXLOCK | O_NONBLOCK : 0));
	if (fd < 0) { perror(o.image); return 1; }
	if (isdev) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	else fcntl(fd, F_NOCACHE, 1);
	FILE *csv = NULL;
	if (out) { if (!nh_path_allowed(out)) { fprintf(stderr, "refused out path\n"); return 1; } csv = fopen(out, "w"); if (csv) fprintf(csv, "i,write_s,flush_s\n"); }
	nh_bwdev b; memset(&b, 0, sizeof b); b.fd = fd; b.logfd = -1; b.fullsync = 1; b.isdev = isdev;
	unsigned char *buf; posix_memalign((void **)&buf, 4096, bs);
	double *wt = calloc(count, sizeof *wt), *ft = calloc(count, sizeof *ft), *tt = calloc(count, sizeof *tt);
	long slots = (region_mib << 20) / bs; uint64_t base = (uint64_t)off_mib << 20; int ferr = 0;
	for (long i = 0; i < count; i++) {
		double t0 = nh_now();
		for (long k = 0; k < burst; k++) {
			uint64_t slot = (uint64_t)(i * burst + k) % slots, off = base + slot * bs;
			nh_fill(buf, bs, 0x5336 + i * burst + k, off);
			if (pwrite(fd, buf, bs, off) != bs) { perror("pwrite"); return 1; }
		}
		double t1 = nh_now(); int r = 0;
		if (!strcmp(mode, "fsync")) r = fsync(fd);
		else if (!strcmp(mode, "fullfsync")) r = fcntl(fd, F_FULLFSYNC, 0);
		else if (!strcmp(mode, "dkioc")) r = ioctl(fd, DKIOCSYNCHRONIZECACHE);
		else if (!strcmp(mode, "barrier")) r = nh_raw_barrier(&b);
		else if (strcmp(mode, "none")) { fprintf(stderr, "bad mode\n"); return 2; }
		if (r == -1 && !ferr++) fprintf(stderr, "flush %s failed: %s\n", mode, strerror(errno));
		double t2 = nh_now();
		wt[i] = t1 - t0; ft[i] = t2 - t1; tt[i] = t2 - t0;
		if (csv) fprintf(csv, "%ld,%.6f,%.6f\n", i, wt[i], ft[i]);
	}
	if (csv) fclose(csv);
	printf("flushbench target=%s isdev=%d mode=%s bs=%ld burst=%ld count=%ld flush_errors=%d\n", o.image, isdev, mode, bs, burst, count, ferr);
	summary("flushbench write", wt, count); summary("flushbench flush", ft, count); summary("flushbench total", tt, count);
	close(fd); free(buf); free(wt); free(ft); free(tt);
	return ferr ? 1 : 0;
}

int cmd_commitlat(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *kind = "create", *out = NULL; long count = 1000;
	for (int i = 1; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--count")) count = atol(argv[++i]);
		else if (!strcmp(argv[i], "--kind")) kind = argv[++i];
		else if (!strcmp(argv[i], "--out")) out = argv[++i];
	}
	o.rw = 1; o.wal = 1; o.commit_ms = -1;
	int err; ntfs_volume *vol = nh_mount(&o, 0, &err);
	if (!vol) { fprintf(stderr, "mount failed: %s\n", strerror(err)); return 3; }
	if (NVolReadOnly(vol)) { fprintf(stderr, "refused: read-only\n"); nh_umount(vol); return 30; }
	nh_bwdev *b = nh_dev_of(vol);
	char dir[64]; snprintf(dir, sizeof dir, "/lat%ld", (long)getpid());
	int r = nh_mkdir(vol, dir);
	if (r) { fprintf(stderr, "mkdir: %s\n", strerror(-r)); nh_umount(vol); return 1; }
	nh_commit(vol, -1);
	FILE *csv = NULL;
	if (out) { if (!nh_path_allowed(out)) { fprintf(stderr, "refused out path\n"); nh_umount(vol); return 1; } csv = fopen(out, "w"); if (csv) fprintf(csv, "i,commit_s,syncs,b1_skipped,blocks\n"); }
	double *ct = calloc(count, sizeof *ct); unsigned char data[4096];
	uint64_t s0 = b->nsyncs, sk0 = b->wal_b1_skipped;
	for (long i = 0; i < count; i++) {
		char p[128]; snprintf(p, sizeof p, "%s/f%06ld", dir, i);
		if (!strcmp(kind, "data")) {
			ntfs_inode *ni; ntfs_attr *na;
			if ((r = nh_open_data(vol, p, NULL, 1, &ni, &na))) break;
			nh_fill(data, sizeof data, i, 0);
			if (nh_pwrite_all(na, 0, sizeof data, data) != sizeof data) { r = -EIO; nh_close_data(ni, na, 1); break; }
			if ((r = nh_close_data(ni, na, 1))) break;
		} else {
			ntfs_inode *ni; if ((r = nh_create(vol, p, S_IFREG, &ni))) break;
			ntfs_inode_close(ni);
		}
		uint64_t sy = b->nsyncs, sk = b->wal_b1_skipped, bl = b->wal_txn_blocks;
		double t = nh_now();
		if ((r = nh_commit(vol, i))) break;
		ct[i] = nh_now() - t;
		if (csv) fprintf(csv, "%ld,%.6f,%llu,%llu,%llu\n", i, ct[i], (unsigned long long)(b->nsyncs - sy), (unsigned long long)(b->wal_b1_skipped - sk), (unsigned long long)(b->wal_txn_blocks - bl));
		if (r) break;
	}
	uint64_t syncs = b->nsyncs - s0, skipped = b->wal_b1_skipped - sk0;
	if (csv) fclose(csv);
	int ur = nh_umount(vol);
	if (r) { fprintf(stderr, "commitlat: op failed: %s\n", strerror(-r)); return 1; }
	printf("commitlat target=%s kind=%s barriers=%s count=%ld syncs=%llu b1_skipped=%llu syncs_per_commit=%.2f umount=%d\n", o.image, kind,
	       o.one_barrier ? "adaptive" : "2", count, (unsigned long long)syncs, (unsigned long long)skipped, (double)syncs / count, ur);
	summary("commitlat commit", ct, count);
	free(ct);
	return ur ? 1 : 0;
}

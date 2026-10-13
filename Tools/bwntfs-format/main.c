/*
 * bwntfs-format - makes an NTFS volume with a Branchway journal on ONE device the caller names.
 *
 *   bwntfs-format -L LABEL [--journal-mib N] [--mkntfs PATH] [--dry-run] TARGET
 *
 * TARGET is either
 *   - a partition node the caller names, /dev/diskNsM or /dev/rdiskNsM, on an external disk,
 *     not mounted (the app unmounts through Disk Arbitration first), or
 *   - a regular image file (tests, and the app's "format an image" path), laid out as partition 1
 *     of an MBR disk (hidden sectors 2048, as the spike's images).
 * The tool never picks, derives or widens a target: no whole-disk nodes (/dev/diskN), no symlinks,
 * no relative paths, no internal disks, nothing another process holds open.
 *
 * Steps: validate -> mkntfs -Q -L LABEL (quick format, explicit geometry) -> bw_journal_init
 * (/$BwJournal, contiguous, superblock) -> bw_fsck (read-only walk) as the proof.
 *
 * Exit codes: 0 done, 2 usage, 3 refused (target is not allowed), 4 busy (a process holds it),
 * 5 mkntfs failed, 6 journal init failed, 7 the check after formatting failed.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOPartitionScheme.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "bwntfs.h"

extern char **environ;

enum { EX_OK_ = 0, EX_USAGE_ = 2, EX_REFUSED = 3, EX_BUSY = 4, EX_MKNTFS = 5, EX_JOURNAL = 6, EX_VERIFY = 7 };
#define IMAGE_HIDDEN_SECTORS 2048
#define MIN_BYTES (16ULL << 20)

static int refuse(int code, const char *fmt, const char *arg)
{
	fprintf(stderr, "bwntfs-format: refused: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	return code;
}

/* ---- target description ---- */
typedef struct {
	const char *path;
	int is_device;
	char bsd[64];           /* diskNsM for devices */
	char whole[64];         /* diskN */
	uint64_t bytes;
	uint32_t sector;
	uint64_t hidden;        /* partition offset in sectors (mkntfs -p) */
} target;

/* "/dev/diskNsM" or "/dev/rdiskNsM" -> bsd "diskNsM", whole "diskN"; 0 when the name is not a partition node */
static int parse_partition_node(const char *path, char *bsd, size_t bn, char *whole, size_t wn)
{
	const char *p;
	if (!strncmp(path, "/dev/rdisk", 10)) p = path + 6; else if (!strncmp(path, "/dev/disk", 9)) p = path + 5; else return 0;
	const char *q = p + 4;
	if (*q < '0' || *q > '9') return 0;
	while (*q >= '0' && *q <= '9') q++;
	size_t wl = (size_t)(q - p);
	if (*q != 's') return 0;   /* /dev/diskN: a whole disk */
	const char *r = q + 1;
	if (*r < '1' || *r > '9') return 0;
	while (*r >= '0' && *r <= '9') r++;
	if (*r) return 0;          /* diskNsMsK (APFS volumes) and anything else */
	if (strlen(p) >= bn || wl >= wn) return 0;
	strlcpy(bsd, p, bn);
	memcpy(whole, p, wl); whole[wl] = 0;
	return 1;
}

static int cf_bool(CFDictionaryRef d, CFStringRef key, int dflt)
{
	CFTypeRef v = d ? CFDictionaryGetValue(d, key) : NULL;
	return v && CFGetTypeID(v) == CFBooleanGetTypeID() ? CFBooleanGetValue((CFBooleanRef)v) : dflt;
}

/* Disk Arbitration (documented API): a partition (not whole media), external, writable, not mounted.
 * The partition offset comes from the IOMedia "Base" property (IOKit/storage/IOPartitionScheme.h:53 kIOMediaBaseKey). */
static int check_device(target *t)
{
	DASessionRef s = DASessionCreate(kCFAllocatorDefault);
	if (!s) return refuse(EX_REFUSED, "%s: no Disk Arbitration session", t->path);
	DADiskRef d = DADiskCreateFromBSDName(kCFAllocatorDefault, s, t->bsd);
	CFDictionaryRef desc = d ? DADiskCopyDescription(d) : NULL;
	int rc = 0;
	if (!desc) rc = refuse(EX_REFUSED, "%s: no such disk", t->path);
	else if (cf_bool(desc, kDADiskDescriptionMediaWholeKey, 1)) rc = refuse(EX_REFUSED, "%s is a whole disk; name one partition", t->path);
	else if (cf_bool(desc, kDADiskDescriptionDeviceInternalKey, 1)) rc = refuse(EX_REFUSED, "%s is on an internal disk", t->path);
	else if (!cf_bool(desc, kDADiskDescriptionMediaWritableKey, 0)) rc = refuse(EX_REFUSED, "%s is read-only media", t->path);
	else if (CFDictionaryGetValue(desc, kDADiskDescriptionVolumePathKey)) rc = refuse(EX_BUSY, "%s is mounted; unmount it first", t->path);
	if (!rc) {
		io_service_t m = DADiskCopyIOMedia(d);
		CFTypeRef base = m ? IORegistryEntryCreateCFProperty(m, CFSTR(kIOMediaBaseKey), kCFAllocatorDefault, 0) : NULL;
		uint64_t b = 0;
		if (!base || CFGetTypeID(base) != CFNumberGetTypeID() || !CFNumberGetValue((CFNumberRef)base, kCFNumberSInt64Type, &b))
			rc = refuse(EX_REFUSED, "%s: partition offset unknown", t->path);
		if (base) CFRelease(base);
		if (m) IOObjectRelease(m);
		if (!rc) t->hidden = b / (t->sector ? t->sector : 512);
	}
	if (desc) CFRelease(desc);
	if (d) CFRelease(d);
	CFRelease(s);
	return rc;
}

/* any process holding PATH open (lsof -t prints pids; exit 1 + no output = none). Fails closed. */
static int held_open(const char *path)
{
	int pfd[2];
	if (pipe(pfd)) return -1;
	posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&fa, pfd[0]);
	char *argv[] = { "/usr/sbin/lsof", "-t", "--", (char *)path, NULL };
	pid_t pid;
	int e = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pfd[1]);
	if (e) { close(pfd[0]); return -1; }
	char buf[256]; ssize_t n, total = 0;
	while ((n = read(pfd[0], buf, sizeof buf)) > 0) total += n;
	close(pfd[0]);
	int st; if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return -1;
	if (total > 0) return 1;
	return WEXITSTATUS(st) <= 1 ? 0 : -1;
}

static int check_not_held(const target *t)
{
	char nodes[4][80]; const char *list[4]; int n = 0;
	if (t->is_device) {
		snprintf(nodes[0], 80, "/dev/%s", t->bsd); snprintf(nodes[1], 80, "/dev/r%s", t->bsd);
		snprintf(nodes[2], 80, "/dev/%s", t->whole); snprintf(nodes[3], 80, "/dev/r%s", t->whole);
		for (; n < 4; n++) list[n] = nodes[n];
	} else list[n++] = t->path;
	for (int i = 0; i < n; i++) {
		int h = held_open(list[i]);
		if (h < 0) return refuse(EX_BUSY, "%s: could not check who holds it (lsof failed)", list[i]);
		if (h > 0) return refuse(EX_BUSY, "%s is open in another process", list[i]);
	}
	return 0;
}

static int validate(target *t)
{
	const char *p = t->path;
	if (p[0] != '/') return refuse(EX_REFUSED, "%s: name the target by its absolute path", p);
	struct stat ls;
	if (lstat(p, &ls)) return refuse(EX_REFUSED, "%s: no such file or device", p);
	if (S_ISLNK(ls.st_mode)) return refuse(EX_REFUSED, "%s is a symbolic link; name the target itself", p);
	char rp[PATH_MAX];
	if (!realpath(p, rp) || strcmp(rp, p)) return refuse(EX_REFUSED, "%s: not a canonical path", p);
	if (!strncmp(p, "/dev/", 5)) {
		if (!parse_partition_node(p, t->bsd, sizeof t->bsd, t->whole, sizeof t->whole))
			return refuse(EX_REFUSED, "%s is not a partition node (/dev/diskNsM)", p);
		if (!S_ISBLK(ls.st_mode) && !S_ISCHR(ls.st_mode)) return refuse(EX_REFUSED, "%s is not a device node", p);
		t->is_device = 1;
		int fd = open(p, O_RDONLY);
		if (fd < 0) return refuse(EX_REFUSED, "%s: cannot open for reading", p);
		uint64_t cnt = 0; uint32_t bs = 0;
		int bad = ioctl(fd, DKIOCGETBLOCKCOUNT, &cnt) || ioctl(fd, DKIOCGETBLOCKSIZE, &bs) || !bs;
		close(fd);
		if (bad) return refuse(EX_REFUSED, "%s: block geometry unknown", p);
		t->bytes = cnt * bs; t->sector = bs;
		int rc = check_device(t);
		if (rc) return rc;
	} else {
		if (!S_ISREG(ls.st_mode)) return refuse(EX_REFUSED, "%s is not a regular image file", p);
		t->bytes = (uint64_t)ls.st_size; t->sector = 512; t->hidden = IMAGE_HIDDEN_SECTORS;
	}
	if (t->bytes < MIN_BYTES) return refuse(EX_REFUSED, "%s is smaller than 16 MiB", p);
	return check_not_held(t);
}

/* ---- the image / device backend for the engine ---- */
typedef struct { int fd; uint32_t bs; int isdev; } fio;

static int64_t f_pread(void *c, void *b, int64_t n, int64_t off)
{
	fio *f = c;
	if (!f->isdev || (off % f->bs == 0 && n % f->bs == 0)) { ssize_t r = pread(f->fd, b, (size_t)n, off); return r < 0 ? -errno : r; }
	int64_t lo = off - off % f->bs, hi = (off + n + f->bs - 1) / f->bs * f->bs;
	uint8_t *t = malloc((size_t)(hi - lo)); if (!t) return -ENOMEM;
	ssize_t r = pread(f->fd, t, (size_t)(hi - lo), lo);
	if (r < 0) { free(t); return -errno; }
	int64_t got = r - (off - lo); if (got > n) got = n; if (got < 0) got = 0;
	memcpy(b, t + (off - lo), (size_t)got); free(t);
	return got;
}

static int64_t f_pwrite(void *c, const void *b, int64_t n, int64_t off)
{
	fio *f = c;
	if (!f->isdev || (off % f->bs == 0 && n % f->bs == 0)) { ssize_t r = pwrite(f->fd, b, (size_t)n, off); return r < 0 ? -errno : r; }
	int64_t lo = off - off % f->bs, hi = (off + n + f->bs - 1) / f->bs * f->bs;
	uint8_t *t = malloc((size_t)(hi - lo)); if (!t) return -ENOMEM;
	if (pread(f->fd, t, (size_t)(hi - lo), lo) != hi - lo) { free(t); return -EIO; }
	memcpy(t + (off - lo), b, (size_t)n);
	ssize_t r = pwrite(f->fd, t, (size_t)(hi - lo), lo); free(t);
	return r == hi - lo ? n : -(errno ? errno : EIO);
}

static int f_barrier(void *c)
{
	fio *f = c;
	if (!fcntl(f->fd, F_FULLFSYNC)) return 0;
	return fsync(f->fd) ? -errno : 0;
}

static int open_io(const target *t, int writable, fio *f, bw_io *io)
{
	f->fd = open(t->path, writable ? O_RDWR : O_RDONLY);
	if (f->fd < 0) return -errno;
	f->bs = t->sector; f->isdev = t->is_device;
	memset(io, 0, sizeof *io);
	io->ctx = f; io->pread = f_pread; io->pwrite = f_pwrite; io->barrier = f_barrier;
	io->size = t->bytes; io->block_size = t->sector; io->writable = writable;
	return 0;
}

/* ---- mkntfs ---- */
static int find_mkntfs(const char *given, char *out, size_t n)
{
	if (given) { strlcpy(out, given, n); return access(out, X_OK); }
	const char *env = getenv("BWNTFS_MKNTFS");
	if (env && *env) { strlcpy(out, env, n); return access(out, X_OK); }
	char exe[PATH_MAX]; uint32_t sz = sizeof exe;
	if (_NSGetExecutablePath(exe, &sz)) return -1;
	char rp[PATH_MAX]; if (!realpath(exe, rp)) return -1;
	char *dir = dirname(rp);
	snprintf(out, n, "%s/mkntfs", dir); if (!access(out, X_OK)) return 0;                 /* next to the tool (app bundle) */
	snprintf(out, n, "%s/libntfs-3g/sbin/mkntfs", dir); return access(out, X_OK);       /* build/ layout */
}

static int run_mkntfs(const char *mk, const target *t, const char *label)
{
	char sec[32], hid[32], cnt[32];
	snprintf(sec, sizeof sec, "%u", t->sector);
	snprintf(hid, sizeof hid, "%llu", (unsigned long long)t->hidden);
	snprintf(cnt, sizeof cnt, "%llu", (unsigned long long)(t->bytes / t->sector));
	/* -Q quick (no zeroing, no bad-sector scan); -F because geometry is given explicitly and mkntfs
	 * cannot read it on macOS; -H/-S are the conventional 255/63 (Windows ignores them on LBA disks). */
	char *argv[] = { (char *)mk, "-Q", "-F", "-c", "4096", "-s", sec, "-p", hid, "-H", "255", "-S", "63",
	                 "-L", (char *)label, (char *)t->path, cnt, NULL };
	pid_t pid; int st;
	if (posix_spawn(&pid, mk, NULL, NULL, argv, environ)) return -1;
	if (waitpid(pid, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void usage(void)
{
	fprintf(stderr, "usage: bwntfs-format -L LABEL [--journal-mib N] [--mkntfs PATH] [--dry-run] TARGET\n"
	                "  TARGET: /dev/diskNsM (an unmounted partition on an external disk) or an image file\n");
}

int main(int argc, char **argv)
{
	const char *label = NULL, *mk_given = NULL, *path = NULL;
	int jmib = 32, dry = 0;
	for (int i = 1; i < argc; i++) {
		if ((!strcmp(argv[i], "-L") || !strcmp(argv[i], "--label")) && i + 1 < argc) label = argv[++i];
		else if (!strcmp(argv[i], "--journal-mib") && i + 1 < argc) jmib = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--mkntfs") && i + 1 < argc) mk_given = argv[++i];
		else if (!strcmp(argv[i], "--dry-run")) dry = 1;
		else if (argv[i][0] == '-') { usage(); return EX_USAGE_; }
		else if (!path) path = argv[i];
		else { fprintf(stderr, "bwntfs-format: exactly one target\n"); return EX_USAGE_; }
	}
	if (!path || !label) { usage(); return EX_USAGE_; }
	size_t ll = strlen(label);
	if (!ll || ll > 128) { fprintf(stderr, "bwntfs-format: label must be 1..128 characters\n"); return EX_USAGE_; }
	if (jmib < 1 || jmib > 1024) { fprintf(stderr, "bwntfs-format: --journal-mib 1..1024\n"); return EX_USAGE_; }

	target t; memset(&t, 0, sizeof t); t.path = path;
	int rc = validate(&t);
	if (rc) return rc;
	if ((uint64_t)jmib << 20 > t.bytes / 4) return refuse(EX_REFUSED, "%s: the journal would take more than a quarter of the volume", path);
	char mk[PATH_MAX];
	if (find_mkntfs(mk_given, mk, sizeof mk)) { fprintf(stderr, "bwntfs-format: mkntfs not found (--mkntfs or BWNTFS_MKNTFS)\n"); return EX_USAGE_; }
	printf("plan target=%s kind=%s bytes=%llu sector=%u hidden=%llu label=%s journal_mib=%d mkntfs=%s\n", path,
	       t.is_device ? "partition" : "image", (unsigned long long)t.bytes, t.sector, (unsigned long long)t.hidden, label, jmib, mk);
	if (dry) { printf("dry-run: nothing written\n"); return 0; }

	int m = run_mkntfs(mk, &t, label);
	if (m) { fprintf(stderr, "bwntfs-format: mkntfs failed (%d)\n", m); return EX_MKNTFS; }

	fio f; bw_io io;
	if ((rc = open_io(&t, 1, &f, &io))) { fprintf(stderr, "bwntfs-format: reopen: %s\n", strerror(-rc)); return EX_JOURNAL; }
	rc = bw_journal_init(&io, path, jmib);
	if (!rc) rc = f_barrier(&f);
	close(f.fd);
	if (rc) { fprintf(stderr, "bwntfs-format: journal init failed: %s\n", strerror(-rc)); return EX_JOURNAL; }

	if ((rc = open_io(&t, 0, &f, &io))) { fprintf(stderr, "bwntfs-format: reopen: %s\n", strerror(-rc)); return EX_VERIFY; }
	bw_fsck_result fr;
	rc = bw_fsck(&io, path, BW_FSCK_CHECK, &fr);
	close(f.fd);
	printf("check %s\n", fr.summary);
	if (rc || fr.errors) { fprintf(stderr, "bwntfs-format: check after formatting failed\n"); return EX_VERIFY; }
	printf("done target=%s label=%s journal_mib=%d\n", path, label, jmib);
	return 0;
}

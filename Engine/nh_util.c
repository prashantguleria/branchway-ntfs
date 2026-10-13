/* nh_util.c - shared helpers and the command dispatcher. */
#include "nh_internal.h"
#include "nhcore.h"
#include <stdarg.h>
#include <libgen.h>

#ifndef NH_LIBNTFS_VERSION
#define NH_LIBNTFS_VERSION "unknown"
#endif

const char *nh_libntfs_version(void) { return NH_LIBNTFS_VERSION; }

double nh_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void nh_sha_hex(const unsigned char *md, char out[65])
{
	static const char *hx = "0123456789abcdef";
	for (int i = 0; i < 32; i++) { out[2 * i] = hx[md[i] >> 4]; out[2 * i + 1] = hx[md[i] & 15]; }
	out[64] = 0;
}

/* "/a/b/c" -> parent "/a/b", leaf "c"; "/c" -> parent "/" */
int nh_split(const char *path, char *parent, size_t plen, const char **leaf)
{
	const char *slash = strrchr(path, '/');
	if (!slash || !slash[1]) return -EINVAL;
	size_t n = slash - path;
	if (n == 0) { strlcpy(parent, "/", plen); }
	else { if (n >= plen) return -ENAMETOOLONG; memcpy(parent, path, n); parent[n] = 0; }
	*leaf = slash + 1;
	return 0;
}

uint64_t nh_rand(uint64_t *s)
{
	/* splitmix64 */
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* Deterministic content: depends on (seed, absolute offset), so any range can be regenerated. */
void nh_fill(unsigned char *buf, size_t len, uint64_t seed, uint64_t off)
{
	size_t i = 0;
	while (i < len) {
		uint64_t blk = (off + i) / 8, s = seed ^ (blk * 0x9e3779b97f4a7c15ULL);
		uint64_t v = nh_rand(&s);
		size_t within = (off + i) % 8;
		for (; within < 8 && i < len; within++, i++) buf[i] = (unsigned char)(v >> (8 * within));
	}
}

int nh_uname(const char *s, ntfschar **u)
{
	*u = NULL;
	int n = ntfs_mbstoucs(s, u);
	if (n < 0) return -errno;
	if (n > NTFS_MAX_NAME_LEN) { free(*u); *u = NULL; return -ENAMETOOLONG; }
	return n;
}

ntfs_inode *nh_lookup(ntfs_volume *vol, const char *path)
{
	return ntfs_pathname_to_inode(vol, NULL, path);
}

const char *nh_errstr(int e) { return strerror(e < 0 ? -e : e); }

int nh_jprint(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	int r = vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	return r;
}

int nh_dev_allowed(const char *path)
{
	const char *env = getenv("NH_ALLOW_DEVICE");
	if (!path || !env || strcmp(env, path)) return 0;
	const char *p = path;
	if (!strncmp(p, "/dev/rdisk", 10)) p += 10; else if (!strncmp(p, "/dev/disk", 9)) p += 9; else return 0;
	if (*p < '0' || *p > '9') return 0;
	while (*p >= '0' && *p <= '9') p++;
	return !strcmp(p, "s1");
}

#include <sys/disk.h>
int nh_open_target(const char *path, int flags)
{
	int isdev = nh_dev_allowed(path);
	if (!isdev && !nh_path_allowed(path)) { fprintf(stderr, "refused: %s is outside %s\n", path, NH_ALLOWED_ROOT); errno = EPERM; return -1; }
	int fd = open(path, flags);
	if (fd < 0 || !isdev) return fd;
	uint64_t cnt = 0; uint32_t bs = 0;
	if (ioctl(fd, DKIOCGETBLOCKCOUNT, &cnt) || ioctl(fd, DKIOCGETBLOCKSIZE, &bs) || cnt * bs != NH_DEVICE_BYTES) {
		fprintf(stderr, "refused: %s reports %llu bytes, expected %llu (not the authorised stick)\n", path,
		        (unsigned long long)cnt * bs, (unsigned long long)NH_DEVICE_BYTES);
		close(fd); errno = EPERM; return -1;
	}
	return fd;
}

int nh_path_allowed(const char *path)
{
	char rp[PATH_MAX];
	if (!path || strncmp(path, "/dev/", 5) == 0) return 0;
	if (!realpath(path, rp)) {
		/* not existing yet: resolve the parent */
		char tmp[PATH_MAX], *d;
		strlcpy(tmp, path, sizeof tmp);
		d = dirname(tmp);
		if (!realpath(d, rp)) return 0;
		strlcat(rp, "/", sizeof rp);
	} else {
		struct stat st;
		if (!stat(rp, &st) && S_ISDIR(st.st_mode)) strlcat(rp, "/", sizeof rp);
	}
	/* /Volumes/Media may resolve through /System/Volumes/Data on some setups */
	return strncmp(rp, NH_ALLOWED_ROOT, strlen(NH_ALLOWED_ROOT)) == 0;
}

/* Parses leading global options and removes them from argv. */
int nh_opts_parse(nh_opts *o, int *argc, char ***argv)
{
	memset(o, 0, sizeof *o);
	o->fullsync = 1;
	o->dirty_guard = 1;
	o->ro_guard = 1;
	o->wal = -1;
	int ac = *argc; char **av = *argv;
	int out = 1;
	for (int i = 1; i < ac; i++) {
		if (!strcmp(av[i], "--image") && i + 1 < ac) o->image = av[++i];
		else if (!strcmp(av[i], "--rw")) o->rw = 1;
		else if (!strcmp(av[i], "--nocache")) o->nocache = 1;
		else if (!strcmp(av[i], "--fsync")) o->fullsync = 0;
		else if (!strcmp(av[i], "--wlog") && i + 1 < ac) o->wlog = av[++i];
		else if (!strcmp(av[i], "--recover")) o->mflags |= NTFS_MNT_RECOVER;
		else if (!strcmp(av[i], "--ignore-hiberfile")) o->mflags |= NTFS_MNT_IGNORE_HIBERFILE;
		else if (!strcmp(av[i], "--may-rdonly")) o->mflags |= NTFS_MNT_MAY_RDONLY;
		else if (!strcmp(av[i], "--quiet")) o->quiet = 1;
		else if (!strcmp(av[i], "--dirty-guard")) o->dirty_guard = 1;   /* default; kept for old scripts */
		else if (!strcmp(av[i], "--no-dirty-guard")) o->dirty_guard = 0;
		else if (!strcmp(av[i], "--force-dirty-rw")) o->ro_guard = 0;
		else if (!strcmp(av[i], "--no-wal")) o->wal = 0;
		else if (!strcmp(av[i], "--commit") && i + 1 < ac) {
			const char *c = av[++i];
			o->commit_ms = !strcmp(c, "op") ? 0 : !strcmp(c, "close") ? -1 : !strncmp(c, "ms=", 3) ? atoi(c + 3) : 0;
		}
		else if (!strcmp(av[i], "--wal")) o->wal = 1;
		else if (!strcmp(av[i], "--clear-own-dirty")) o->clear_own_dirty = 1;
		else if (!strcmp(av[i], "--barriers") && i + 1 < ac) { const char *c = av[++i]; o->one_barrier = !strcmp(c, "adaptive"); }
		/* libntfs-3g on macOS (ENABLE_NFCONV) composes names to NFC on input and
		 * decomposes to NFD on output; --raw-names turns both off */
		else if (!strcmp(av[i], "--raw-names")) ntfs_macosx_normalize_filenames(0);
		else av[out++] = av[i];
	}
	*argc = out;
	return 0;
}

struct cmdent { const char *name; int (*fn)(int, char **); const char *help; };
static const struct cmdent cmds[] = {
	{ "ls", cmd_ls, "ls --image IMG PATH [-l]" },
	{ "stat", cmd_stat, "stat --image IMG PATH" },
	{ "mkdir", cmd_mkdir, "mkdir --image IMG --rw PATH" },
	{ "write", cmd_write, "write --image IMG --rw SRCFILE PATH [--stream NAME]" },
	{ "read", cmd_read, "read --image IMG PATH DSTFILE [--stream NAME]  (prints sha256)" },
	{ "sha", cmd_sha, "sha --image IMG PATH [--stream NAME]" },
	{ "rename", cmd_rename, "rename --image IMG --rw FROM TO" },
	{ "rm", cmd_rm, "rm --image IMG --rw PATH" },
	{ "truncate", cmd_truncate, "truncate --image IMG --rw PATH SIZE" },
	{ "setattr", cmd_setattr, "setattr --image IMG --rw PATH [--attrib HEX] [--mtime EPOCH] [--atime EPOCH] [--crtime EPOCH]" },
	{ "statfs", cmd_statfs, "statfs --image IMG" },
	{ "flags", cmd_flags, "flags --image IMG  (cheap dirty/hibernated probe, read-only)" },
	{ "probe", cmd_probe, "probe IMG  (raw boot sector + $Volume flags + hiberfil, no mount)" },
	{ "setflag", cmd_setflag, "setflag --image IMG --rw dirty|clean|hiberfile|hiberfile-clear" },
	{ "mounttest", cmd_mounttest, "mounttest --image IMG  (try rw, rw+may-rdonly, ro; report)" },
	{ "fsck", cmd_fsck, "fsck --image IMG [--no-data]  (MFT/bitmap/index/link/orphan walk)" },
	{ "tree-gen", cmd_treegen, "tree-gen --image IMG --rw --manifest FILE [--files N]" },
	{ "verify", cmd_verify, "verify --image IMG --manifest FILE [--completed-op N] [--root PATH] | verify --posix DIR --manifest FILE" },
	{ "bench", cmd_bench, "bench (--image IMG --rw | --posix DIR) --workload NAME [--manifest FILE] [--chunk N] [--size N] [--count N] [--sync-every N]" },
	{ "crashimg", cmd_crashimg, "crashimg BASE LOG OUT --cut N [--reorder SEED] [--torn]" },
	{ "wloginfo", cmd_wloginfo, "wloginfo LOG" },
	{ "jinit", cmd_jinit, "jinit --image IMG [--size MiB]  (create the /$BwJournal journal file)" },
	{ "jinfo", cmd_jinfo, "jinfo IMG  (raw: journal superblock + pending transactions, read-only)" },
	{ "recover", cmd_recover, "recover --image IMG [--clear-own-dirty]  (replay the journal in place, no mount)" },
	{ "flushbench", cmd_flushbench, "flushbench --image T --mode none|fsync|fullfsync|dkioc|barrier [--count N] [--bs B] [--burst K] [--region-mib M] [--offset-mib O] [--out CSV]  (S6: raw write+flush latency; DESTROYS the region)" },
	{ "commitlat", cmd_commitlat, "commitlat --image IMG [--barriers adaptive] [--kind create|data] [--count N] [--out CSV]  (S6: journal commit latency)" },
	{ NULL, NULL, NULL }
};

static void usage(void)
{
	fprintf(stderr, "ntfsharness (libntfs-3g %s)\n", NH_LIBNTFS_VERSION);
	for (int i = 0; cmds[i].name; i++) fprintf(stderr, "  %s\n", cmds[i].help);
}

int nh_cmd(int argc, char **argv)
{
	if (argc < 1) { usage(); return 2; }
	ntfs_log_set_handler(ntfs_log_handler_stderr);
	if (getenv("NH_DEBUG")) ntfs_log_set_levels(NTFS_LOG_LEVEL_DEBUG | NTFS_LOG_LEVEL_TRACE | NTFS_LOG_LEVEL_VERBOSE);
	for (int i = 0; cmds[i].name; i++)
		if (!strcmp(argv[0], cmds[i].name)) return cmds[i].fn(argc, argv);
	usage();
	return 2;
}

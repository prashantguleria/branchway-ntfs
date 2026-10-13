/*
 * nh_tree.c - tree-gen: a Windows-like tree on an image, written through
 * libntfs-3g, with a manifest of every regular file's unnamed-stream content
 * (verify reads it back). What it builds, all under /tree:
 *   names/    255-unit ASCII and CJK names, NFC and NFD twins, emoji, case
 *             variants, Win32-invalid characters, trailing dot and space
 *   many/     N small files (default 10000), sizes 0..8191
 *   deep/     nested directories past 260 and 1024 characters of path
 *   ads/      Zone.Identifier stream, a 1 MiB stream, a stream on a directory
 *   links/    symlinks (relative, absolute, dangling), hard links, DOS names
 *   reparse/  files tagged WOF (0x80000017) and cloud (0x9000001A)
 *   comp/     a compressed directory with compressible files
 *   sparse/   a sparse file with data at 0 and 64 MiB
 *   attrs/    hidden, system, read-only; extreme timestamps
 *   frag/     an 8 MiB file written through 4 KiB holes; a file with 300 streams
 *             (attribute list)
 */
#include "nh_internal.h"

static ntfs_volume *V;
static int errors;
static unsigned char *G;	/* scratch buffer */

#define TRY(x) do { int _r = (x); if (_r) { errors++; fprintf(stderr, "tree-gen: %s: %s (line %d)\n", #x, nh_errstr(_r), __LINE__); } } while (0)

/* create (or replace) a file with bytes, record it in the manifest */
static int tw(const char *path, const void *data, s64 len, int record)
{
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(V, path, NULL, 1, &ni, &na);
	if (r) return r;
	if (len && nh_pwrite_all(na, 0, len, data) != len) r = -EIO;
	int r2 = nh_close_data(ni, na, 1); if (!r) r = r2;
	if (!r && record) {
		unsigned char md[32]; char hex[65];
		CC_SHA256(data, (CC_LONG)len, md); nh_sha_hex(md, hex);
		nh_mcreate(path, len, hex);
	}
	return r;
}
static int tgen(const char *path, s64 len, uint64_t seed)
{
	nh_fill(G, len, seed, 0);
	return tw(path, G, len, 1);
}
static int mk(const char *path) { int r = nh_mkdir(V, path); return r == -EEXIST ? 0 : r; }

static int set_attrib(const char *path, u32 add)
{
	ntfs_inode *ni = nh_lookup(V, path); if (!ni) return -errno;
	le32 at = ni->flags | cpu_to_le32(add);
	int r = ntfs_set_ntfs_attrib(ni, (const char *)&at, sizeof at, 0) ? -errno : 0;
	if (ntfs_inode_close(ni) && !r) r = -errno;
	return r;
}
static int stream(const char *path, const char *sname, const void *data, s64 len)
{
	ntfs_inode *ni; ntfs_attr *na;
	int r = nh_open_data(V, path, sname, 1, &ni, &na);
	if (r) return r;
	if (nh_pwrite_all(na, 0, len, data) != len) r = -EIO;
	int r2 = nh_close_data(ni, na, 1);
	return r ? r : r2;
}
static int symlink_at(const char *path, const char *target)
{
	char parent[NH_PATHMAX]; const char *leaf;
	int r = nh_split(path, parent, sizeof parent, &leaf); if (r) return r;
	ntfschar *u, *t; int ul = nh_uname(leaf, &u), tl = nh_uname(target, &t);
	if (ul < 0 || tl < 0) return -EINVAL;
	ntfs_inode *dir = nh_lookup(V, parent);
	if (!dir) { r = -errno; free(u); free(t); return r; }
	ntfs_inode *ni = ntfs_create_symlink(dir, const_cpu_to_le32(0), u, ul, t, tl);
	if (!ni) r = -errno;
	else if (ntfs_inode_close_in_dir(ni, dir)) r = -errno;
	if (ntfs_inode_close(dir) && !r) r = -errno;
	free(u); free(t);
	return r;
}
static int hardlink(const char *from, const char *to)
{
	char parent[NH_PATHMAX]; const char *leaf;
	int r = nh_split(to, parent, sizeof parent, &leaf); if (r) return r;
	ntfschar *u; int ul = nh_uname(leaf, &u); if (ul < 0) return ul;
	ntfs_inode *ni = nh_lookup(V, from); if (!ni) { free(u); return -errno; }
	ntfs_inode *dir = nh_lookup(V, parent); if (!dir) { r = -errno; ntfs_inode_close(ni); free(u); return r; }
	if (ntfs_link(ni, dir, u, ul)) r = -errno;
	if (ntfs_inode_close(dir) && !r) r = -errno;
	if (ntfs_inode_close(ni) && !r) r = -errno;
	free(u);
	return r;
}
static int dosname(const char *path, const char *dos)
{
	char parent[NH_PATHMAX]; const char *leaf;
	int r = nh_split(path, parent, sizeof parent, &leaf); if (r) return r;
	ntfs_inode *ni = nh_lookup(V, path); if (!ni) return -errno;
	ntfs_inode *dir = nh_lookup(V, parent); if (!dir) { r = -errno; ntfs_inode_close(ni); return r; }
	/* always closes both */
	return ntfs_set_ntfs_dos_name(ni, dir, dos, strlen(dos), 0) ? -errno : 0;
}
static int reparse(const char *path, u32 tag, const void *data, u16 dlen)
{
	unsigned char buf[512];
	le32 t = cpu_to_le32(tag); le16 l = cpu_to_le16(dlen), z = 0;
	memcpy(buf, &t, 4); memcpy(buf + 4, &l, 2); memcpy(buf + 6, &z, 2); memcpy(buf + 8, data, dlen);
	ntfs_inode *ni = nh_lookup(V, path); if (!ni) return -errno;
	int r = ntfs_set_ntfs_reparse_data(ni, (const char *)buf, 8 + dlen, 0) ? -errno : 0;
	if (ntfs_inode_close(ni) && !r) r = -errno;
	return r;
}
static int set_times(const char *path, u64 cr, u64 mt, u64 at)
{
	ntfs_inode *ni = nh_lookup(V, path); if (!ni) return -errno;
	u64 t[3] = { cr, mt, at };
	int r = ntfs_inode_set_times(ni, (const char *)t, sizeof t, 0) ? -errno : 0;
	if (ntfs_inode_close(ni) && !r) r = -errno;
	return r;
}

/* append a UTF-8 encoding of code point cp */
static char *u8put(char *p, uint32_t cp)
{
	if (cp < 0x80) *p++ = cp;
	else if (cp < 0x800) { *p++ = 0xC0 | cp >> 6; *p++ = 0x80 | (cp & 63); }
	else if (cp < 0x10000) { *p++ = 0xE0 | cp >> 12; *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
	else { *p++ = 0xF0 | cp >> 18; *p++ = 0x80 | (cp >> 12 & 63); *p++ = 0x80 | (cp >> 6 & 63); *p++ = 0x80 | (cp & 63); }
	return p;
}

static int has(int argc, char **argv, const char *k) { for (int i = 1; i < argc; i++) if (!strcmp(argv[i], k)) return 1; return 0; }
static const char *val(int argc, char **argv, const char *k) { for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], k)) return argv[i + 1]; return NULL; }

int cmd_treegen(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	const char *man = val(argc, argv, "--manifest"), *nf = val(argc, argv, "--files");
	long nfiles = nf ? atol(nf) : 10000;
	if (!man || !o.rw) { fprintf(stderr, "tree-gen --image IMG --rw --manifest FILE [--files N] [--no-frag]\n"); return 2; }
	if (!nh_path_allowed(man)) { fprintf(stderr, "refused manifest path\n"); return 1; }
	int mfd = open(man, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (mfd < 0) { perror(man); return 1; }
	nh_mopen(mfd);
	ntfs_macosx_normalize_filenames(0);	/* raw names: the NFC and NFD twins must both reach the disk */
	int err; V = nh_mount(&o, 0, &err);
	if (!V) { fprintf(stderr, "mount failed: %s\n", strerror(err)); return 3; }
	if (NVolReadOnly(V)) { fprintf(stderr, "refused: volume mounted read-only by the guard (EROFS)\n"); nh_umount(V); return 30; }
	G = malloc(16 << 20);
	double t0 = nh_now();
	char p[NH_PATHMAX * 2], q[NH_PATHMAX * 2], *w;
	TRY(mk("/tree"));

	/* ---- names ---- */
	TRY(mk("/tree/names"));
	strcpy(p, "/tree/names/"); w = p + strlen(p); for (int i = 0; i < 251; i++) *w++ = 'a' + i % 26; strcpy(w, ".txt");
	TRY(tgen(p, 100, 1));
	strcpy(p, "/tree/names/"); w = p + strlen(p); for (int i = 0; i < 255; i++) w = u8put(w, 0x4E00 + i); *w = 0; /* 255 units, 765 bytes */
	TRY(tgen(p, 100, 2));
	TRY(tgen("/tree/names/caf\xC3\xA9.txt", 10, 3));		/* NFC */
	TRY(tgen("/tree/names/cafe\xCC\x81.txt", 11, 4));		/* NFD, a different name on NTFS */
	strcpy(p, "/tree/names/"); w = p + strlen(p); w = u8put(w, 0x1F4C1); w += sprintf(w, " folder "); w = u8put(w, 0x1F389); w = u8put(w, 0x1F468); w = u8put(w, 0x200D); w = u8put(w, 0x1F469); strcpy(w, ".txt");
	TRY(tgen(p, 12, 5));
	TRY(tgen("/tree/names/README.txt", 13, 6));
	TRY(tgen("/tree/names/Readme.TXT", 14, 7));			/* case twin: legal POSIX namespace, a Windows conflict */
	TRY(tgen("/tree/names/readme.txt", 15, 8));
	TRY(tgen("/tree/names/colon:in?name*\"<>|.txt", 16, 9));	/* Win32-invalid */
	TRY(tgen("/tree/names/trailing dot.", 17, 10));
	TRY(tgen("/tree/names/trailing space ", 18, 11));
	TRY(tgen("/tree/names/ leading space", 19, 12));
	TRY(tgen("/tree/names/CON", 20, 13));				/* reserved device name */
	TRY(tgen("/tree/names/\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xE4\xBD\xA0\xE5\xA5\xBD \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7.docx", 21, 14));
	TRY(mk("/tree/names/Dir With Spaces"));
	TRY(tgen("/tree/names/Dir With Spaces/file (1).txt", 22, 15));

	/* ---- many ---- */
	TRY(mk("/tree/many"));
	for (long i = 0; i < nfiles; i++) {
		if (i % 1000 == 0) { snprintf(q, sizeof q, "/tree/many/d%02ld", i / 1000); TRY(mk(q)); }
		snprintf(p, sizeof p, "%s/file_%05ld.dat", q, i);
		TRY(tgen(p, (s64)((i * 2654435761u) % 8192), 1000 + i));
	}

	/* ---- deep: 60 levels of 19-char names = 1200+ chars ---- */
	strcpy(p, "/tree/deep"); TRY(mk(p));
	for (int i = 0; i < 60; i++) {
		size_t l = strlen(p); snprintf(p + l, sizeof p - l, "/level_%02d_abcdefghij", i); TRY(mk(p));
		if (i == 13 || i == 59) { snprintf(q, sizeof q, "%s/leaf.txt", p); TRY(tgen(q, 333, 77 + i)); }
	}

	/* ---- ADS ---- */
	TRY(mk("/tree/ads"));
	TRY(tgen("/tree/ads/downloaded.exe", 4096, 20));
	const char *zone = "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.com/\r\n";
	TRY(stream("/tree/ads/downloaded.exe", "Zone.Identifier", zone, strlen(zone)));
	TRY(tgen("/tree/ads/big-stream.bin", 1000, 21));
	nh_fill(G, 1 << 20, 22, 0); TRY(stream("/tree/ads/big-stream.bin", "payload", G, 1 << 20));
	TRY(mk("/tree/ads/dir-with-stream"));
	TRY(stream("/tree/ads/dir-with-stream", "meta", "directory stream", 16));
	strcpy(p, ""); w = p; w = u8put(w, 0xF000); w += sprintf(w, "favicon");	/* Mac-style private-use stream name */
	TRY(stream("/tree/ads/downloaded.exe", p, "x", 1));

	/* ---- links ---- */
	TRY(mk("/tree/links"));
	TRY(tgen("/tree/links/target.txt", 500, 30));
	TRY(symlink_at("/tree/links/rel-link", "target.txt"));
	TRY(symlink_at("/tree/links/abs-link", "/tree/links/target.txt"));
	TRY(symlink_at("/tree/links/dangling", "does-not-exist"));
	TRY(symlink_at("/tree/links/dir-link", "../names"));
	TRY(hardlink("/tree/links/target.txt", "/tree/links/hardlink-1.txt"));
	TRY(hardlink("/tree/links/target.txt", "/tree/names/hardlink-elsewhere.txt"));
	nh_fill(G, 500, 30, 0);
	{ unsigned char md[32]; char hex[65]; CC_SHA256(G, 500, md); nh_sha_hex(md, hex);
	  nh_mcreate("/tree/links/hardlink-1.txt", 500, hex); nh_mcreate("/tree/names/hardlink-elsewhere.txt", 500, hex); }
	TRY(tgen("/tree/links/A Long File Name.document", 40, 31));
	TRY(dosname("/tree/links/A Long File Name.document", "ALONGF~1.DOC"));

	/* ---- reparse tags ---- */
	TRY(mk("/tree/reparse"));
	TRY(tw("/tree/reparse/wof-compressed.dll", "", 0, 0));
	{ unsigned char wof[16] = { 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 }; /* WOF v1, provider FILE, v1, XPRESS8K */
	  TRY(reparse("/tree/reparse/wof-compressed.dll", 0x80000017, wof, sizeof wof)); }
	TRY(tw("/tree/reparse/onedrive-placeholder.docx", "", 0, 0));
	{ unsigned char cl[24] = { 0x01, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44 };
	  TRY(reparse("/tree/reparse/onedrive-placeholder.docx", 0x9000001A, cl, sizeof cl)); }
	nh_mcreate("/tree/reparse/wof-compressed.dll", 0, "-");
	nh_mcreate("/tree/reparse/onedrive-placeholder.docx", 0, "-");
	nh_mcreate("/tree/links/rel-link", 0, "-"); nh_mcreate("/tree/links/abs-link", 0, "-");
	nh_mcreate("/tree/links/dangling", 0, "-"); nh_mcreate("/tree/links/dir-link", 0, "-");

	/* ---- compressed directory ---- */
	TRY(mk("/tree/comp"));
	TRY(set_attrib("/tree/comp", FILE_ATTR_COMPRESSED));
	if (V->cluster_size <= 4096) {
		NVolSetCompression(V);
		for (int i = 0; i < 20; i++) {
			s64 len = 4096 + i * 37000;
			for (s64 j = 0; j < len; j++) G[j] = "the quick brown fox jumps over the lazy dog\n"[j % 44];
			snprintf(p, sizeof p, "/tree/comp/log-%02d.txt", i);
			TRY(tw(p, G, len, 1));
		}
		NVolClearCompression(V);
	} else printf("tree-gen: cluster %u > 4096: compression not possible, comp/ left empty\n", V->cluster_size);

	/* ---- sparse ---- */
	TRY(mk("/tree/sparse"));
	{
		ntfs_inode *ni; ntfs_attr *na;
		TRY(nh_open_data(V, "/tree/sparse/vm-disk.img", NULL, 1, &ni, &na));
		s64 sz = (64 << 20) + 4096;
		CC_SHA256_CTX c; CC_SHA256_Init(&c);
		nh_fill(G, 4096, 40, 0); nh_pwrite_all(na, 0, 4096, G);
		nh_fill(G + 4096, 4096, 41, 0); nh_pwrite_all(na, 64 << 20, 4096, G + 4096);
		CC_SHA256_Update(&c, G, 4096);
		memset(G + 8192, 0, 1 << 20);
		for (s64 off = 4096; off < 64 << 20; ) { s64 n = (64 << 20) - off < (1 << 20) ? (64 << 20) - off : (1 << 20); CC_SHA256_Update(&c, G + 8192, (CC_LONG)n); off += n; }
		CC_SHA256_Update(&c, G + 4096, 4096);
		unsigned char md[32]; char hex[65]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex);
		printf("tree-gen: sparse file flags=0x%x allocated=%lld size=%lld\n", le32_to_cpu(ni->flags), (long long)na->allocated_size, (long long)na->data_size);
		TRY(nh_close_data(ni, na, 1));
		nh_mcreate("/tree/sparse/vm-disk.img", sz, hex);
	}

	/* ---- attributes and timestamps ---- */
	TRY(mk("/tree/attrs"));
	TRY(tgen("/tree/attrs/hidden.txt", 10, 50)); TRY(set_attrib("/tree/attrs/hidden.txt", FILE_ATTR_HIDDEN));
	TRY(tgen("/tree/attrs/system.sys", 10, 51)); TRY(set_attrib("/tree/attrs/system.sys", FILE_ATTR_SYSTEM | FILE_ATTR_HIDDEN));
	TRY(tgen("/tree/attrs/readonly.txt", 10, 52)); TRY(set_attrib("/tree/attrs/readonly.txt", FILE_ATTR_READONLY));
	TRY(tgen("/tree/attrs/time-1601.txt", 10, 53)); TRY(set_times("/tree/attrs/time-1601.txt", 1, 1, 1));
	TRY(tgen("/tree/attrs/time-1970.txt", 10, 54)); TRY(set_times("/tree/attrs/time-1970.txt", 116444736000000000ULL, 116444736000000000ULL, 116444736000000000ULL));
	TRY(tgen("/tree/attrs/time-2400.txt", 10, 55)); TRY(set_times("/tree/attrs/time-2400.txt", 252465408000000000ULL, 252465408000000000ULL, 252465408000000000ULL));
	TRY(tgen("/tree/attrs/time-max.txt", 10, 56)); TRY(set_times("/tree/attrs/time-max.txt", 0x7FFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL));

	/* ---- fragmented file and attribute lists ---- */
	if (!has(argc, argv, "--no-frag")) {
		TRY(mk("/tree/frag")); TRY(mk("/tree/frag/fill"));
		/* punch free space into 4 KiB holes, then write a file through them */
		nh_fill(G, 4096, 0, 0);
		for (int i = 0; i < 4000; i++) { snprintf(p, sizeof p, "/tree/frag/fill/h%04d", i); TRY(tw(p, G, 4096, 0)); }
		for (int i = 0; i < 4000; i += 2) { snprintf(p, sizeof p, "/tree/frag/fill/h%04d", i); TRY(nh_unlink(V, p)); }
		for (int i = 1; i < 4000; i += 2) { snprintf(p, sizeof p, "/tree/frag/fill/h%04d", i); nh_fill(G, 4096, 0, 0); unsigned char md[32]; char hex[65]; CC_SHA256(G, 4096, md); nh_sha_hex(md, hex); nh_mcreate(p, 4096, hex); }
		ntfs_inode *ni; ntfs_attr *na; CC_SHA256_CTX c; CC_SHA256_Init(&c);
		const s64 total = 8 << 20, step = 4096;
		TRY(nh_open_data(V, "/tree/frag/fragmented.bin", NULL, 1, &ni, &na));
		for (s64 off = 0; off < total; off += step) { nh_fill(G, step, 60, off); if (nh_pwrite_all(na, off, step, G) != step) errors++; CC_SHA256_Update(&c, G, step); }
		/* libntfs-3g allocates forward from its last position, so it does not refill the holes:
		 * this file stays one run; the stream-heavy file below is the attribute-list case */
		printf("tree-gen: fragmented.bin attr_list=%d\n", NInoAttrList(ni) ? 1 : 0);
		TRY(nh_close_data(ni, na, 1));
		{ unsigned char md[32]; char hex[65]; CC_SHA256_Final(md, &c); nh_sha_hex(md, hex); nh_mcreate("/tree/frag/fragmented.bin", total, hex); }
		/* 300 named streams on one file always overflow the base record */
		TRY(tgen("/tree/frag/many-streams.txt", 100, 61));
		for (int i = 0; i < 300; i++) { char sn[32]; snprintf(sn, sizeof sn, "s%03d", i); nh_fill(G, 200, 62 + i, 0); TRY(stream("/tree/frag/many-streams.txt", sn, G, 200)); }
		ni = nh_lookup(V, "/tree/frag/many-streams.txt");
		if (ni) { printf("tree-gen: many-streams.txt attr_list=%d\n", NInoAttrList(ni) ? 1 : 0); ntfs_inode_close(ni); }
	}

	int ur = nh_umount(V);
	close(mfd);
	printf("tree-gen result=%s errors=%d files=%ld secs=%.1f\n", errors || ur ? "FAIL" : "PASS", errors, nfiles, nh_now() - t0);
	free(G);
	return errors || ur ? 5 : 0;
}

/*
 * nh_fsck.c - a read-only structural checker built on libntfs-3g.
 *
 * ntfsfix -n only checks $MFT/$MFTMirr, $UpCase, the boot sector and $LogFile.
 * This walks the whole volume:
 *   1. every MFT record: in-use flag vs $MFT:$BITMAP, fixups, base/extent;
 *   2. every non-resident attribute run: in range, never owned twice, marked
 *      in $Bitmap (a used cluster that is free in $Bitmap is an ERROR, a set
 *      bit with no owner is a leak WARNING);
 *   3. every $FILE_NAME: parent in use and a directory, and indexed in that
 *      parent; every index entry: target in use, sequence number current,
 *      matching $FILE_NAME, and found again by a B-tree lookup;
 *   4. orphans: in-use user records with no name;
 *   5. (unless --no-data) every $DATA stream of user files read end to end.
 */
#include "nh_internal.h"

typedef struct { uint64_t *keys; size_t cap, n; uint8_t *hit; } kset;
static void ks_init(kset *s, size_t cap) { s->cap = 1; while (s->cap < cap * 2) s->cap <<= 1; s->keys = calloc(s->cap, 8); s->hit = calloc(s->cap, 1); s->n = 0; }
static size_t ks_slot(kset *s, uint64_t k) { if (!k) k = 1; size_t i = (k * 0x9e3779b97f4a7c15ULL) & (s->cap - 1); while (s->keys[i] && s->keys[i] != k) i = (i + 1) & (s->cap - 1); return i; }
static void ks_add(kset *s, uint64_t k) { if (!k) k = 1; if (s->n * 2 >= s->cap) { kset t; ks_init(&t, s->cap); for (size_t i = 0; i < s->cap; i++) if (s->keys[i]) { size_t j = ks_slot(&t, s->keys[i]); t.keys[j] = s->keys[i]; t.hit[j] = s->hit[i]; t.n++; } free(s->keys); free(s->hit); *s = t; } size_t i = ks_slot(s, k); if (!s->keys[i]) { s->keys[i] = k; s->n++; } }
static int ks_mark(kset *s, uint64_t k) { if (!k) k = 1; size_t i = ks_slot(s, k); if (!s->keys[i]) return 0; s->hit[i] = 1; return 1; }

static uint64_t name_key(uint64_t child, uint64_t parent, const ntfschar *nm, int len, int type)
{
	uint64_t h = 1469598103934665603ULL ^ (child * 31 + parent * 1000003 + (uint64_t)type * 7);
	for (int i = 0; i < len; i++) { h ^= le16_to_cpu(nm[i]); h *= 1099511628211ULL; }
	h ^= child << 1; h *= 1099511628211ULL; h ^= parent; h *= 1099511628211ULL;
	return h;
}

static int nerr, nwarn;
#define FERR(...) do { if (nerr++ < 40) { printf("ERROR "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define FWARN(...) do { if (nwarn++ < 20) { printf("WARN "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct dirent_rec { ntfschar *name; int len; int type; MFT_REF mref; };
struct dctx { struct dirent_rec *v; size_t n, cap; };
static int collect_cb(void *d, const ntfschar *name, const int len, const int type, const s64 pos, const MFT_REF mref, const unsigned dt)
{
	struct dctx *c = d; (void)pos; (void)dt;
	if (len == 1 && le16_to_cpu(name[0]) == '.') return 0;
	if (len == 2 && le16_to_cpu(name[0]) == '.' && le16_to_cpu(name[1]) == '.') return 0;
	if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 256; c->v = realloc(c->v, c->cap * sizeof *c->v); }
	c->v[c->n].name = malloc(len * sizeof(ntfschar)); memcpy(c->v[c->n].name, name, len * sizeof(ntfschar));
	c->v[c->n].len = len; c->v[c->n].type = type; c->v[c->n].mref = mref; c->n++;
	return 0;
}

int cmd_fsck(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	int nodata = 0;
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-data")) nodata = 1;
	o.rw = 0;
	int err; ntfs_volume *vol = nh_mount(&o, 0, &err);
	if (!vol) { printf("ERROR mount failed: %s\nfsck result=FAIL errors=1\n", strerror(err)); return 4; }
	int ne = 0, nw = 0;
	nh_fsck_vol(vol, nodata, &ne, &nw, NULL, NULL);
	nh_umount(vol);
	return ne ? 4 : 0;
}

/* the walk itself, on a mounted volume (read-only use; the caller mounts and unmounts) */
int nh_fsck_vol(ntfs_volume *vol, int nodata, int *perr, int *pwarn, uint64_t *pfiles, uint64_t *pdirs)
{
	nerr = nwarn = 0;
	double t0 = nh_now();
	s64 nrec = vol->mft_na->initialized_size >> vol->mft_record_size_bits;
	s64 ncl = vol->nr_clusters;
	/* load $MFT:$BITMAP and $Bitmap */
	s64 mbsz = vol->mftbmp_na->data_size, cbsz = vol->lcnbmp_na->data_size;
	u8 *mftbmp = calloc(1, mbsz + 8), *lcnbmp = calloc(1, cbsz + 8), *owned = calloc(1, (ncl + 7) / 8 + 8);
	if (ntfs_attr_pread(vol->mftbmp_na, 0, mbsz, mftbmp) != mbsz) FERR("cannot read $MFT:$BITMAP");
	if (ntfs_attr_pread(vol->lcnbmp_na, 0, cbsz, lcnbmp) != cbsz) FERR("cannot read $Bitmap");

	u8 *inuse = calloc(nrec, 1), *isdir = calloc(nrec, 1), *isbase = calloc(nrec, 1);
	u16 *seq = calloc(nrec, 2); u16 *fncount = calloc(nrec, 2), *linkcnt = calloc(nrec, 2);
	kset names; ks_init(&names, 4096);
	typedef struct { u64 child, parent; } pref; size_t npref = 0, cpref = 1024; pref *prefs = malloc(cpref * sizeof *prefs);
	u64 *dirs = malloc(sizeof(u64) * 1024); size_t ndirs = 0, cdirs = 1024;
	u64 nbase = 0, nextent = 0, nattrs = 0, nnonres = 0, owned_cl = 0, datastreams = 0;
	s64 databytes = 0;
	MFT_RECORD *m = ntfs_malloc(vol->mft_record_size);

	for (s64 i = 0; i < nrec; i++) {
		int bit = ntfs_bit_get(mftbmp, i);
		if (ntfs_mft_records_read(vol, MK_MREF(i, 0), 1, m)) {
			if (bit) FERR("record %lld: unreadable (%s)", (long long)i, strerror(errno));
			continue;
		}
		int iu = !!(m->flags & MFT_RECORD_IN_USE);
		seq[i] = le16_to_cpu(m->sequence_number);
		if (iu && !bit) FERR("record %lld: in use but free in $MFT bitmap", (long long)i);
		if (!iu && bit && i >= 24) FWARN("record %lld: set in $MFT bitmap but not in use (leak)", (long long)i);
		if (!iu) continue;
		if (ntfs_mft_record_check(vol, MK_MREF(i, 0), m)) { FERR("record %lld: fails record check", (long long)i); continue; }
		inuse[i] = 1;
		if (m->base_mft_record) { nextent++; continue; }
		isbase[i] = 1; nbase++;
		linkcnt[i] = le16_to_cpu(m->link_count);
		if (m->flags & MFT_RECORD_IS_DIRECTORY) { isdir[i] = 1; if (ndirs == cdirs) { cdirs *= 2; dirs = realloc(dirs, cdirs * 8); } dirs[ndirs++] = i; }
	}

	/* attributes of every base inode */
	for (s64 i = 0; i < nrec; i++) {
		if (!isbase[i]) continue;
		ntfs_inode *ni = ntfs_inode_open(vol, i);
		if (!ni) { FERR("inode %lld: open failed (%s)", (long long)i, strerror(errno)); continue; }
		ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
		struct { ntfschar *nm; int len; } streams[64]; int nstreams = 0;
		int r;
		while (!(r = ntfs_attr_lookup(AT_UNUSED, NULL, 0, CASE_SENSITIVE, 0, NULL, 0, ctx))) {
			ATTR_RECORD *a = ctx->attr;
			nattrs++;
			if (a->type == AT_FILE_NAME) {
				FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)a + le16_to_cpu(a->value_offset));
				u64 par = MREF_LE(fn->parent_directory);
				ks_add(&names, name_key(i, par, fn->file_name, fn->file_name_length, fn->file_name_type));
				if (fn->file_name_type != FILE_NAME_DOS) fncount[i]++;
				if (npref == cpref) { cpref *= 2; prefs = realloc(prefs, cpref * sizeof *prefs); }
				prefs[npref].child = i; prefs[npref].parent = par; npref++;
				if (i != FILE_root && (u16)MSEQNO_LE(fn->parent_directory) && par < (u64)nrec && seq[par] != (u16)MSEQNO_LE(fn->parent_directory))
					FERR("inode %lld: $FILE_NAME parent %llu seq %u != record seq %u", (long long)i, (unsigned long long)par, (unsigned)MSEQNO_LE(fn->parent_directory), seq[par]);
			}
			if (a->type == AT_DATA && nstreams < 64) {
				streams[nstreams].len = a->name_length;
				streams[nstreams].nm = a->name_length ? malloc(a->name_length * 2) : NULL;
				if (a->name_length) memcpy(streams[nstreams].nm, (u8 *)a + le16_to_cpu(a->name_offset), a->name_length * 2);
				/* skip duplicates (extents of the same stream) */
				int dup = 0;
				for (int k = 0; k < nstreams; k++) if (streams[k].len == streams[nstreams].len && (!a->name_length || !memcmp(streams[k].nm, streams[nstreams].nm, a->name_length * 2))) dup = 1;
				if (dup) free(streams[nstreams].nm); else nstreams++;
			}
			if (a->non_resident) {
				nnonres++;
				runlist_element *rl = ntfs_mapping_pairs_decompress(vol, a, NULL);
				if (!rl) { FERR("inode %lld: attr 0x%x: bad mapping pairs", (long long)i, le32_to_cpu(a->type)); continue; }
				for (runlist_element *e = rl; e->length; e++) {
					if (e->lcn < 0) continue;
					if (e->lcn + e->length > ncl) { FERR("inode %lld: run %lld+%lld beyond volume", (long long)i, (long long)e->lcn, (long long)e->length); continue; }
					for (s64 c = e->lcn; c < e->lcn + e->length; c++) {
						if (ntfs_bit_get(owned, c)) FERR("cluster %lld: owned twice (inode %lld attr 0x%x)", (long long)c, (long long)i, le32_to_cpu(a->type));
						else { ntfs_bit_set(owned, c, 1); owned_cl++; }
						if (!ntfs_bit_get(lcnbmp, c)) FERR("cluster %lld: used by inode %lld but free in $Bitmap", (long long)c, (long long)i);
					}
				}
				free(rl);
			}
		}
		if (errno != ENOENT) FERR("inode %lld: attribute walk failed (%s)", (long long)i, strerror(errno));
		ntfs_attr_put_search_ctx(ctx);
		if (!nodata && i >= FILE_first_user) {
			static unsigned char buf[1 << 20];
			for (int k = 0; k < nstreams; k++) {
				ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, streams[k].len ? streams[k].nm : AT_UNNAMED, streams[k].len);
				if (!na) { FERR("inode %lld: cannot open $DATA stream %d (%s)", (long long)i, k, strerror(errno)); continue; }
				datastreams++;
				if (ni->flags & FILE_ATTR_ENCRYPTED) { ntfs_attr_close(na); continue; }
				for (s64 off = 0; off < na->data_size; ) {
					s64 n = na->data_size - off > (s64)sizeof buf ? (s64)sizeof buf : na->data_size - off;
					s64 got = ntfs_attr_pread(na, off, n, buf);
					if (got <= 0) { FERR("inode %lld: read of stream %d failed at %lld (%s)", (long long)i, k, (long long)off, strerror(errno)); break; }
					off += got; databytes += got;
				}
				ntfs_attr_close(na);
			}
		}
		for (int k = 0; k < nstreams; k++) free(streams[k].nm);
		ntfs_inode_close(ni);
	}
	/* parents */
	for (size_t k = 0; k < npref; k++) {
		u64 p = prefs[k].parent;
		if (p >= (u64)nrec || !inuse[p] || !isdir[p]) FERR("inode %llu: parent %llu not an in-use directory", (unsigned long long)prefs[k].child, (unsigned long long)p);
	}
	/* directory indexes */
	u64 nentries = 0;
	for (size_t d = 0; d < ndirs; d++) {
		ntfs_inode *dn = ntfs_inode_open(vol, dirs[d]);
		if (!dn) { FERR("dir %llu: open failed", (unsigned long long)dirs[d]); continue; }
		struct dctx c = { 0 }; s64 pos = 0;
		if (ntfs_readdir(dn, &pos, &c, collect_cb)) FERR("dir %llu: readdir failed (%s)", (unsigned long long)dirs[d], strerror(errno));
		for (size_t k = 0; k < c.n; k++) {
			struct dirent_rec *e = &c.v[k];
			u64 ch = MREF(e->mref); u16 sq = MSEQNO(e->mref);
			nentries++;
			if (ch >= (u64)nrec || !inuse[ch]) { FERR("dir %llu: entry -> record %llu not in use", (unsigned long long)dirs[d], (unsigned long long)ch); continue; }
			if (sq && sq != seq[ch]) FERR("dir %llu: entry -> record %llu stale seq %u (now %u)", (unsigned long long)dirs[d], (unsigned long long)ch, sq, seq[ch]);
			if (!ks_mark(&names, name_key(ch, dirs[d], e->name, e->len, e->type)))
				FERR("dir %llu: entry -> record %llu has no matching $FILE_NAME", (unsigned long long)dirs[d], (unsigned long long)ch);
			if (e->type != FILE_NAME_DOS) {
				u64 got = ntfs_inode_lookup_by_name(dn, e->name, e->len);
				if (got == (u64)-1 || MREF(got) != ch) FERR("dir %llu: B-tree lookup of entry -> %llu failed", (unsigned long long)dirs[d], (unsigned long long)ch);
			}
			free(e->name);
		}
		free(c.v);
		ntfs_inode_close(dn);
	}
	/* $FILE_NAMEs not found in their parent's index */
	u64 unindexed = 0;
	for (size_t k = 0; k < names.cap; k++) if (names.keys[k] && !names.hit[k]) unindexed++;
	/* root's own $FILE_NAME is "." in itself and is not listed by readdir */
	if (unindexed > 1) FERR("%llu $FILE_NAME attributes are not indexed in their parent", (unsigned long long)(unindexed - 1));
	/* orphans and link counts */
	u64 orphans = 0;
	for (s64 i = FILE_first_user; i < nrec; i++) {
		if (!isbase[i]) continue;
		if (!fncount[i]) { if (i >= 24) { orphans++; FERR("inode %lld: in use with no name (orphan)", (long long)i); } continue; }
		if (linkcnt[i] != fncount[i]) FWARN("inode %lld: link_count %u vs %u names", (long long)i, linkcnt[i], fncount[i]);
	}
	/* cluster leaks */
	u64 leaked = 0;
	for (s64 c = 0; c < ncl; c++) if (ntfs_bit_get(lcnbmp, c) && !ntfs_bit_get(owned, c)) leaked++;
	if (leaked) FWARN("%llu clusters set in $Bitmap with no owner (leak)", (unsigned long long)leaked);
	int dirty = !!(vol->flags & VOLUME_IS_DIRTY);
	printf("fsck result=%s errors=%d warnings=%d records=%lld base=%llu extents=%llu dirs=%zu attrs=%llu nonresident=%llu names=%zu entries=%llu orphans=%llu clusters_owned=%llu clusters_leaked=%llu streams_read=%llu bytes_read=%lld dirty=%d secs=%.2f\n",
	       nerr ? "FAIL" : "PASS", nerr, nwarn, (long long)nrec, (unsigned long long)nbase, (unsigned long long)nextent, ndirs,
	       (unsigned long long)nattrs, (unsigned long long)nnonres, names.n, (unsigned long long)nentries,
	       (unsigned long long)orphans, (unsigned long long)owned_cl, (unsigned long long)leaked,
	       (unsigned long long)datastreams, (long long)databytes, dirty, nh_now() - t0);
	free(m); free(mftbmp); free(lcnbmp); free(owned); free(inuse); free(isdir); free(isbase); free(seq); free(fncount); free(linkcnt); free(prefs); free(dirs);
	free(names.keys); free(names.hit);
	if (perr) *perr = nerr;
	if (pwarn) *pwarn = nwarn;
	if (pfiles) *pfiles = nbase > ndirs ? nbase - ndirs : 0;
	if (pdirs) *pdirs = ndirs;
	return nerr ? -EIO : 0;
}

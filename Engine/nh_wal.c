/*
 * nh_wal.c - ordered, journaled device layer under libntfs-3g (NTFS-1b stage 2).
 *
 * libntfs-3g writes metadata in place with no journal and no barriers. This layer
 * sits in the device operations and makes every metadata change atomic per commit:
 *
 *   - Between commit points, writes go to an in-memory overlay of 4 KiB blocks
 *     (read-modify-write; reads are served from the overlay first).
 *   - Direct-write rule (ordered data): a write whose clusters are all free in the
 *     COMMITTED $Bitmap, and whose blocks are not in the overlay or in an
 *     un-checkpointed journal transaction, goes straight to disk. Such clusters are
 *     unreachable until the transaction that allocates them commits.
 *   - Commit: descriptor + block images -> journal, BARRIER, commit record (+ op
 *     marker in the write log), BARRIER, then the blocks in place (no barrier: the
 *     next commit's first barrier, or the checkpoint, makes them durable).
 *   - Checkpoint (journal full, or close): BARRIER, superblock start := tail.
 *   - Recovery (before libntfs-3g reads anything): replay every consecutive valid
 *     transaction from the superblock start; a read-only open replays into the
 *     overlay instead of the disk.
 *
 * The journal lives in a preallocated contiguous file "/$BwJournal" (jinit). It is
 * found by a raw scan of the MFT (own fixups + mapping-pairs decoder), so recovery
 * never depends on libntfs-3g mounting a damaged volume. Windows and Tuxera do not
 * know this journal: what they see before our replay is reported separately.
 *
 * Journal layout (4 KiB blocks): block 0 superblock (first 512 bytes used), blocks
 * 1..N-1 a circular log of transactions:
 *   desc[ndesc] { "BWJD", idx, seq, ndesc, nblk, cnt, u64 blk[cnt] }
 *   data[nblk]
 *   commit      { "BWJC", seq, ndesc, nblk, crc(desc+data), hcrc } (first 64 bytes)
 */
#include "nh_internal.h"
#include <zlib.h>
#include <sys/disk.h>

#define JB 4096u
#define SB_MAGIC 0x534a5742u /* "BWJS" */
#define JD_MAGIC 0x444a5742u /* "BWJD" */
#define JC_MAGIC 0x434a5742u /* "BWJC" */
#define PER_DESC ((JB - 32) / 8)
#define JNAME "$BwJournal"

/* ---------------- block hash map (key = block number) ---------------- */
typedef struct { uint64_t *k; unsigned char **v; size_t cap, n; } hmap;

static hmap *hm_new(void) { hmap *h = calloc(1, sizeof *h); h->cap = 1024; h->k = calloc(h->cap, 8); h->v = calloc(h->cap, sizeof *h->v); return h; }
static size_t hm_ix(const hmap *h, uint64_t key) { return (size_t)((key * 0x9E3779B97F4A7C15ULL) >> 17) & (h->cap - 1); }
static unsigned char **hm_get(hmap *h, uint64_t key)
{
	for (size_t i = hm_ix(h, key); h->k[i]; i = (i + 1) & (h->cap - 1)) if (h->k[i] == key + 1) return &h->v[i];
	return NULL;
}
static unsigned char **hm_put(hmap *h, uint64_t key)
{
	unsigned char **s = hm_get(h, key);
	if (s) return s;
	if ((h->n + 1) * 2 > h->cap) {
		hmap g = { calloc(h->cap * 2, 8), calloc(h->cap * 2, sizeof *h->v), h->cap * 2, 0 };
		for (size_t i = 0; i < h->cap; i++) if (h->k[i]) {
			size_t j = hm_ix(&g, h->k[i] - 1);
			while (g.k[j]) j = (j + 1) & (g.cap - 1);
			g.k[j] = h->k[i]; g.v[j] = h->v[i]; g.n++;
		}
		free(h->k); free(h->v); *h = g;
	}
	size_t i = hm_ix(h, key);
	while (h->k[i]) i = (i + 1) & (h->cap - 1);
	h->k[i] = key + 1; h->v[i] = NULL; h->n++;
	return &h->v[i];
}
static void hm_clear(hmap *h, int owned)
{
	if (owned) for (size_t i = 0; i < h->cap; i++) if (h->k[i]) free(h->v[i]);
	if (h->cap > 4096) { free(h->k); free(h->v); h->cap = 1024; h->k = calloc(h->cap, 8); h->v = calloc(h->cap, sizeof *h->v); }
	else { memset(h->k, 0, h->cap * 8); memset(h->v, 0, h->cap * sizeof *h->v); }
	h->n = 0;
}
static void hm_free(hmap *h, int owned) { if (!h) return; hm_clear(h, owned); free(h->k); free(h->v); free(h); }
static int cmpu64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t *hm_keys(hmap *h)
{
	uint64_t *ks = malloc((h->n + 1) * 8); size_t m = 0;
	for (size_t i = 0; i < h->cap; i++) if (h->k[i]) ks[m++] = h->k[i] - 1;
	qsort(ks, m, 8, cmpu64);
	return ks;
}

/* ---------------- little-endian helpers ---------------- */
static uint16_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t g64(const uint8_t *p) { return g32(p) | (uint64_t)g32(p + 4) << 32; }
static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

/* ---------------- raw journal discovery (no libntfs) ---------------- */
static int fixup(uint8_t *r, uint32_t rs)
{
	uint16_t uo = g16(r + 4), uc = g16(r + 6);
	if (uo + uc * 2 > rs || uc < 2 || (uint32_t)(uc - 1) * 512 != rs) return -1;
	uint16_t usn = g16(r + uo);
	for (int i = 1; i < uc; i++) {
		uint8_t *e = r + i * 512 - 2;
		if (g16(e) != usn) return -1;
		e[0] = r[uo + 2 * i]; e[1] = r[uo + 2 * i + 1];
	}
	return 0;
}
static int decode_runs(const uint8_t *p, const uint8_t *end, int64_t *lcn, int64_t *len, int max)
{
	int n = 0; int64_t cur = 0;
	while (p < end && *p) {
		int lb = *p & 15, ob = *p >> 4; p++;
		if (!lb || lb > 8 || ob > 8 || p + lb + ob > end || n == max) return -1;
		uint64_t l = 0; for (int i = 0; i < lb; i++) l |= (uint64_t)p[i] << (8 * i);
		p += lb;
		if (!ob) lcn[n] = -1;
		else {
			uint64_t d = 0; for (int i = 0; i < ob; i++) d |= (uint64_t)p[i] << (8 * i);
			if (ob < 8 && (p[ob - 1] & 0x80)) d |= ~0ULL << (8 * ob);
			cur += (int64_t)d; lcn[n] = cur;
		}
		p += ob; len[n++] = (int64_t)l;
	}
	return n;
}
/* finds attribute `type` (unnamed); returns offset or -1 */
static int find_attr(const uint8_t *r, uint32_t rs, uint32_t type, int from)
{
	uint32_t off = from ? (uint32_t)from : g16(r + 0x14);
	while (off + 16 <= rs) {
		uint32_t t = g32(r + off), l = g32(r + off + 4);
		if (t == 0xFFFFFFFFu || l < 16 || off + l > rs) return -1;
		if (t == type && r[off + 9] == 0) return (int)off;
		off += l;
	}
	return -1;
}

typedef struct { uint64_t jdev, jbytes, serial; uint32_t cs; uint64_t nclusters, mft_no; } jloc;

/* raw scan: boot sector -> $MFT runlist -> records 16.. for "/$BwJournal" with one run */
static int locate_journal(nh_bwdev *fd, jloc *L)
{
	uint8_t bs[512];
	if (nh_raw_pread(fd, bs, 512, 0) != 512 || memcmp(bs + 3, "NTFS    ", 8)) return -1;
	uint32_t bps = g16(bs + 0x0B); uint8_t spcb = bs[0x0D];
	uint32_t spc = spcb > 0x80 ? 1u << (256 - spcb) : spcb;
	L->cs = bps * spc;
	int64_t mft_lcn = (int64_t)g64(bs + 0x30);
	int8_t cpm = (int8_t)bs[0x40];
	uint32_t rs = cpm > 0 ? (uint32_t)cpm * L->cs : 1u << (-cpm);
	L->serial = g64(bs + 0x48);
	L->nclusters = g64(bs + 0x28) / spc;
	if (!L->cs || rs < 1024 || rs > 65536) return -1;
	uint8_t *r = malloc(rs);
	if (nh_raw_pread(fd, r, rs, mft_lcn * L->cs) != (ssize_t)rs || memcmp(r, "FILE", 4) || fixup(r, rs)) { free(r); return -1; }
	int a = find_attr(r, rs, 0x80, 0);
	if (a < 0 || !r[a + 8]) { free(r); return -1; }
	int64_t lcn[64], len[64];
	int nr = decode_runs(r + a + g16(r + a + 0x20), r + a + g32(r + a + 4), lcn, len, 64);
	if (nr <= 0) { free(r); return -1; }
	uint64_t recno = 0; int found = 0;
	size_t chunk = 256 * 1024; uint8_t *buf = malloc(chunk);
	for (int ri = 0; ri < nr && !found && recno < 65536; ri++) {
		if (lcn[ri] < 0) { recno += (uint64_t)len[ri] * L->cs / rs; continue; }
		uint64_t bytes = (uint64_t)len[ri] * L->cs, done = 0;
		while (done < bytes && !found && recno < 65536) {
			size_t want = bytes - done < chunk ? bytes - done : chunk;
			if (nh_raw_pread(fd, buf, want, lcn[ri] * L->cs + done) != (ssize_t)want) break;
			for (size_t o = 0; o + rs <= want && !found; o += rs, recno++) {
				uint8_t *m = buf + o;
				if (recno < 16 || memcmp(m, "FILE", 4) || !(g16(m + 0x16) & 1)) continue;
				if (fixup(m, rs)) continue;
				int fa = 0, ok = 0;
				while ((fa = find_attr(m, rs, 0x30, fa)) >= 0) {
					if (!m[fa + 8]) {
						const uint8_t *v = m + fa + g16(m + fa + 0x14);
						if ((g64(v) & 0xFFFFFFFFFFFFULL) == 5 && v[0x40] == strlen(JNAME)) {
							ok = 1;
							for (size_t i = 0; i < strlen(JNAME); i++) if (g16(v + 0x42 + 2 * i) != (uint8_t)JNAME[i]) ok = 0;
						}
					}
					if (ok) break;
					fa += g32(m + fa + 4);
				}
				if (!ok) continue;
				int da = find_attr(m, rs, 0x80, 0);
				if (da < 0 || !m[da + 8] || g64(m + da + 0x10) != 0) continue;
				int64_t jl[4], jn[4];
				int jr = decode_runs(m + da + g16(m + da + 0x20), m + da + g32(m + da + 4), jl, jn, 4);
				if (jr != 1 || jl[0] <= 0) continue;
				L->jdev = (uint64_t)jl[0] * L->cs;
				L->jbytes = g64(m + da + 0x30);
				if ((uint64_t)jn[0] * L->cs < L->jbytes) continue;
				L->mft_no = recno;
				found = 1;
			}
			done += want;
		}
	}
	free(buf); free(r);
	return found ? 0 : -1;
}

/* ---------------- volume stamp (stale-journal detection) ----------------
 * Our journal may only be replayed onto the volume state it was written against. If another
 * system wrote the volume after our crash (Windows, chkdsk), replaying our pending transactions
 * would put old metadata blocks over its newer ones. The superblock therefore carries a stamp of
 * two things this engine never changes but Windows does:
 *   - the LSN field of MFT record 3 ($Volume), header offset 8: NTFS logs every metadata change
 *     and stamps the record with the LSN; chkdsk and every Windows mount that touches the volume
 *     flags (dirty bit) move it. libntfs-3g never writes MFT record LSNs.
 *   - crc32 of the first 8 KiB of $LogFile (record 2): the two restart pages. Windows rewrites
 *     the restart area (current LSN, clean/dirty) on every read-write mount and dismount.
 *     libntfs-3g writes $LogFile only on an NTFS_MNT_RECOVER mount (ntfs_empty_logfile,
 *     libntfs-3g/volume.c:1294-1306) and the module never passes that flag.
 * The stamp is taken from the raw disk at every checkpoint (after the barrier, so it is the
 * committed state) and after recovery. At open, pending transactions are replayed only when the
 * stamp matches; on a mismatch they are discarded and the reason is logged.
 * Limits: a foreign writer that changes neither (another libntfs-3g, for example) is not seen;
 * a crash inside a RECOVER mount's own bookkeeping commit would read as stale. */
typedef struct { uint64_t vol_lsn; uint32_t lf_crc; int ok; } vstamp;
typedef struct { uint32_t cs, rs; int64_t lcn[64], len[64]; int nr; } mftmap;

static int mft_map(nh_bwdev *fd, mftmap *m)
{
	uint8_t bs[512];
	if (nh_raw_pread(fd, bs, 512, 0) != 512 || memcmp(bs + 3, "NTFS    ", 8)) return -1;
	uint32_t bps = g16(bs + 0x0B); uint8_t spcb = bs[0x0D];
	uint32_t spc = spcb > 0x80 ? 1u << (256 - spcb) : spcb;
	m->cs = bps * spc;
	int8_t cpm = (int8_t)bs[0x40];
	m->rs = cpm > 0 ? (uint32_t)cpm * m->cs : 1u << (-cpm);
	if (!m->cs || m->rs < 1024 || m->rs > 65536) return -1;
	uint8_t *r = malloc(m->rs);
	if (nh_raw_pread(fd, r, m->rs, (off_t)((int64_t)g64(bs + 0x30) * m->cs)) != (ssize_t)m->rs || memcmp(r, "FILE", 4) || fixup(r, m->rs)) { free(r); return -1; }
	int a = find_attr(r, m->rs, 0x80, 0);
	m->nr = a < 0 || !r[a + 8] ? -1 : decode_runs(r + a + g16(r + a + 0x20), r + a + g32(r + a + 4), m->lcn, m->len, 64);
	free(r);
	return m->nr > 0 ? 0 : -1;
}
/* reads MFT record `no` (fixups applied); the record must lie inside one run */
static int mft_read(nh_bwdev *fd, const mftmap *m, uint64_t no, uint8_t *r)
{
	uint64_t off = no * m->rs, v = 0;
	for (int i = 0; i < m->nr; i++) {
		uint64_t bytes = (uint64_t)m->len[i] * m->cs;
		if (off < v + bytes) {
			if (m->lcn[i] < 0 || off + m->rs > v + bytes) return -1;
			if (nh_raw_pread(fd, r, m->rs, (off_t)((uint64_t)m->lcn[i] * m->cs + off - v)) != (ssize_t)m->rs) return -1;
			return memcmp(r, "FILE", 4) || fixup(r, m->rs) ? -1 : 0;
		}
		v += bytes;
	}
	return -1;
}
static void vol_stamp(nh_bwdev *fd, vstamp *st)
{
	memset(st, 0, sizeof *st);
	mftmap m; if (mft_map(fd, &m)) return;
	uint8_t *r = malloc(m.rs), *lf = malloc(8192);
	int ok = 0;
	if (!mft_read(fd, &m, 3, r)) { st->vol_lsn = g64(r + 8); ok = 1; }
	if (ok && !mft_read(fd, &m, 2, r)) {
		int a = find_attr(r, m.rs, 0x80, 0);
		int64_t l[8], n[8];
		int nr = a < 0 || !r[a + 8] ? -1 : decode_runs(r + a + g16(r + a + 0x20), r + a + g32(r + a + 4), l, n, 8);
		if (nr > 0 && l[0] > 0 && (uint64_t)n[0] * m.cs >= 8192 && nh_raw_pread(fd, lf, 8192, (off_t)((uint64_t)l[0] * m.cs)) == 8192) {
			st->lf_crc = (uint32_t)crc32(0, lf, 8192); st->ok = 1;
		}
	}
	free(r); free(lf);
}

/* ---------------- superblock ----------------
 * v2 (512 bytes): magic@0 version@4 jblocks@8 start@16 start_seq@24 owner_dirty@32 stamp_ok@36
 * serial@40 vol_lsn@48 lf_crc@56 crc32(0..59)@60. v1 has no stamp (bytes 36..59 zero). */
typedef struct { uint64_t jblocks, start, start_seq, serial; uint32_t owner_dirty, version; vstamp st; } jsb;

static void sb_enc(uint8_t *p, const jsb *s)
{
	memset(p, 0, 512);
	p32(p, SB_MAGIC); p32(p + 4, 2);
	p64(p + 8, s->jblocks); p64(p + 16, s->start); p64(p + 24, s->start_seq);
	p32(p + 32, s->owner_dirty); p32(p + 36, s->st.ok ? 1 : 0); p64(p + 40, s->serial);
	p64(p + 48, s->st.vol_lsn); p32(p + 56, s->st.lf_crc);
	p32(p + 60, (uint32_t)crc32(0, p, 60));
}
static int sb_dec(const uint8_t *p, jsb *s)
{
	uint32_t v = g32(p + 4);
	if (g32(p) != SB_MAGIC || (v != 1 && v != 2) || g32(p + 60) != (uint32_t)crc32(0, p, 60)) return -1;
	s->version = v;
	s->jblocks = g64(p + 8); s->start = g64(p + 16); s->start_seq = g64(p + 24);
	s->owner_dirty = g32(p + 32); s->serial = g64(p + 40);
	memset(&s->st, 0, sizeof s->st);
	if (v >= 2) { s->st.ok = g32(p + 36) & 1; s->st.vol_lsn = g64(p + 48); s->st.lf_crc = g32(p + 56); }
	return 0;
}

/* ---------------- state ---------------- */
struct nh_wal {
	jloc L;
	uint64_t logcap;         /* blocks in the circular area */
	uint64_t start, start_seq, tail, seq, used;
	uint32_t owner_dirty;
	int ro;
	hmap *ovl, *ucp;
	uint64_t ovl_cap;
	unsigned char *cbm;      /* committed $Bitmap, NULL until attached */
	uint64_t cbm_bytes;
	struct { uint64_t dev, len, bm; } *bmr; int nbmr;
	int in_commit;
	int direct_dirty;         /* a direct (new-cluster) write since the last barrier */
	vstamp st;                /* volume stamp written into the superblock (committed state) */
};

static uint64_t jdev_of(struct nh_wal *w, uint64_t pos) { return w->L.jdev + (1 + pos % w->logcap) * JB; }

static int write_sb(nh_bwdev *b)
{
	struct nh_wal *w = b->walp;
	jsb s = { w->logcap + 1, w->start, w->start_seq, w->L.serial, w->owner_dirty, 2, w->st };
	uint8_t p[512]; sb_enc(p, &s);
	return nh_raw_pwrite(b, p, 512, w->L.jdev) == 512 ? 0 : -1;
}

/* writes `n` blocks from buf to circular positions starting at pos (<=2 pwrites) */
static int jwrite(nh_bwdev *b, const unsigned char *buf, uint64_t n, uint64_t pos)
{
	struct nh_wal *w = b->walp;
	while (n) {
		uint64_t p = pos % w->logcap, run = w->logcap - p < n ? w->logcap - p : n;
		if (nh_raw_pwrite(b, buf, run * JB, jdev_of(w, p)) != (s64)(run * JB)) return -1;
		b->wal_jbytes += run * JB;
		buf += run * JB; n -= run; pos += run;
	}
	return 0;
}
static int jread(nh_bwdev *fd, struct nh_wal *w, unsigned char *buf, uint64_t n, uint64_t pos)
{
	while (n) {
		uint64_t p = pos % w->logcap, run = w->logcap - p < n ? w->logcap - p : n;
		if (nh_raw_pread(fd, buf, run * JB, jdev_of(w, p)) != (ssize_t)(run * JB)) return -1;
		buf += run * JB; n -= run; pos += run;
	}
	return 0;
}

/* ---------------- recovery ---------------- */
/* walks consecutive valid transactions from the superblock start. discard = 0: apply them in
 * place (rw) or into the overlay (ro). discard = 1 (stale journal): count them, apply nothing. */
static int recover(nh_bwdev *b, int discard, int restamp)
{
	struct nh_wal *w = b->walp;
	uint64_t pos = w->start, seq = w->start_seq, txns = 0, blks = 0;
	unsigned char *d0 = malloc(JB);
	for (;;) {
		if (jread(b, w, d0, 1, pos)) break;
		if (g32(d0) != JD_MAGIC || g32(d0 + 4) != 0 || g64(d0 + 8) != seq) break;
		uint32_t ndesc = g32(d0 + 16), nblk = g32(d0 + 20);
		if (!nblk || ndesc != (nblk + PER_DESC - 1) / PER_DESC || (uint64_t)ndesc + nblk + 1 > w->logcap) break;
		unsigned char *t = malloc((size_t)(ndesc + nblk + 1) * JB);
		if (jread(b, w, t, ndesc + nblk + 1, pos)) { free(t); break; }
		unsigned char *c = t + (size_t)(ndesc + nblk) * JB;
		int ok = g32(c) == JC_MAGIC && g64(c + 8) == seq && g32(c + 16) == ndesc && g32(c + 20) == nblk &&
		         g32(c + 28) == (uint32_t)crc32(0, c, 28) &&
		         g32(c + 24) == (uint32_t)crc32(0, t, (uInt)((ndesc + nblk) * JB));
		uint64_t *tg = malloc((size_t)nblk * 8); uint32_t k = 0;
		for (uint32_t i = 0; ok && i < ndesc; i++) {
			unsigned char *d = t + (size_t)i * JB;
			uint32_t cnt = g32(d + 24);
			if (g32(d) != JD_MAGIC || g32(d + 4) != i || g64(d + 8) != seq || k + cnt > nblk) ok = 0;
			for (uint32_t j = 0; ok && j < cnt; j++) tg[k++] = g64(d + 32 + 8 * j);
		}
		if (!ok || k != nblk) { free(tg); free(t); break; }
		for (uint32_t i = 0; i < nblk && !discard; i++) {
			unsigned char *src = t + (size_t)(ndesc + i) * JB;
			if (w->ro) { unsigned char **s = hm_put(w->ovl, tg[i]); if (!*s) *s = malloc(JB); memcpy(*s, src, JB); }
			else nh_raw_pwrite(b, src, JB, (s64)(tg[i] * JB));
		}
		free(tg); free(t);
		txns++; blks += nblk; seq++; pos += ndesc + nblk + 1;
	}
	free(d0);
	if (discard) { b->wal_stale_txns = txns; b->wal_stale_blocks = blks; }
	else { b->wal_replayed_txns = txns; b->wal_replayed_blocks = blks; }
	w->tail = pos % w->logcap; w->seq = seq; w->used = 0;
	if (!w->ro) {
		w->start = w->tail; w->start_seq = w->seq;
		if (txns || restamp) {
			/* replayed blocks durable, then the stamp of the state they produced, then the superblock */
			nh_raw_barrier(b);
			vstamp st; vol_stamp(b, &st); if (st.ok) w->st = st;
			write_sb(b); nh_raw_barrier(b);
		}
	}
	return 0;
}

/* ---------------- open / close ---------------- */
/* 0 usable, 1 not found (absent, renamed, or no longer one contiguous run), 2 superblock invalid
 * or the file shorter than the superblock says (truncated) */
static int journal_state(nh_bwdev *fd, jloc *L, jsb *s)
{
	if (locate_journal(fd, L)) return 1;
	uint8_t p[512];
	if (nh_raw_pread(fd, p, 512, (off_t)L->jdev) != 512 || sb_dec(p, s) || s->serial != L->serial || s->jblocks * JB > L->jbytes || s->jblocks < 64) return 2;
	return 0;
}

int nh_wal_probe(const nh_opts *o)
{
	nh_bwdev b; int r = nh_raw_open(&b, o->image, o->io, O_RDONLY);
	if (r) return r;
	jloc L; jsb s;
	int st = journal_state(&b, &L, &s);
	nh_raw_close(&b);
	return st;
}

/*
 * Read-only open: a missing or invalid journal leaves the device unjournaled (nothing is written).
 * Read-write open with the journal required (wal_req > 0): a missing or invalid journal fails the
 * open (ENOENT / EINVAL); nh_mount re-creates the journal first, so this only happens when that
 * failed, and nh_mount then mounts read-only. Auto mode (wal_req < 0): no journal, no error.
 */
int nh_wal_open(nh_bwdev *b, int ro)
{
	jloc L; jsb s;
	int js = journal_state(b, &L, &s);
	if (js) {
		if (js == 2) fprintf(stderr, "wal: /%s found but superblock invalid or file truncated; journal not used\n", JNAME);
		b->wal_journal_state = js;
		if (ro || b->wal_req < 0) return 0;
		return js == 1 ? -ENOENT : -EINVAL;
	}
	struct nh_wal *w = calloc(1, sizeof *w);
	w->L = L; w->logcap = s.jblocks - 1; w->start = s.start; w->start_seq = s.start_seq;
	w->owner_dirty = s.owner_dirty; w->ro = ro;
	w->ovl = hm_new(); w->ucp = hm_new();
	w->ovl_cap = w->logcap / 4;
	b->walp = w; b->wal = 1;
	vstamp cur; vol_stamp(b, &cur);
	w->st = cur.ok ? cur : s.st;
	int stale = 0, restamp = 0;
	if (s.version < 2) { restamp = 1; fprintf(stderr, "wal: superblock v1 carries no volume stamp; stale check skipped\n"); }
	else if (!s.st.ok || !cur.ok) { restamp = cur.ok; fprintf(stderr, "wal: volume stamp unreadable (%s); stale check skipped\n", !cur.ok ? "volume" : "superblock"); }
	else if (s.st.vol_lsn != cur.vol_lsn || s.st.lf_crc != cur.lf_crc) { stale = 1; restamp = 1; }
	/* a stale journal's owner-dirty flag is void: whoever changed the volume owns its dirty bit now */
	if (stale) w->owner_dirty = 0;
	recover(b, stale, restamp);
	if (stale) {
		b->wal_stale = 1;
		if (b->wal_stale_txns)
			fprintf(stderr, "wal: journal is stale: the volume changed after it was written ($Volume LSN %llu -> %llu, $LogFile restart crc %08x -> %08x); "
			        "%llu pending transactions (%llu blocks) discarded, not replayed\n",
			        (unsigned long long)s.st.vol_lsn, (unsigned long long)cur.vol_lsn, s.st.lf_crc, cur.lf_crc,
			        (unsigned long long)b->wal_stale_txns, (unsigned long long)b->wal_stale_blocks);
		else fprintf(stderr, "wal: volume changed since the last checkpoint (another system mounted it); nothing pending; journal re-stamped\n");
	}
	b->wal_owner_dirty_at_open = stale ? 0 : s.owner_dirty;
	return 0;
}

/* committed $Bitmap shadow; enables the direct-write rule */
int nh_wal_attach(nh_bwdev *b, ntfs_volume *vol)
{
	struct nh_wal *w = b->walp;
	if (!w || w->ro) return 0;
	ntfs_attr *na = vol->lcnbmp_na;
	if (ntfs_attr_map_whole_runlist(na)) return -1;
	w->cbm_bytes = (uint64_t)na->data_size;
	w->cbm = calloc(1, w->cbm_bytes + 8);
	int cap = 16; w->bmr = malloc(cap * sizeof *w->bmr); w->nbmr = 0;
	for (runlist_element *rl = na->rl; rl && rl->length; rl++) {
		if (rl->lcn < 0) continue;
		if (w->nbmr == cap) { cap *= 2; w->bmr = realloc(w->bmr, cap * sizeof *w->bmr); }
		uint64_t bm = (uint64_t)rl->vcn * vol->cluster_size, len = (uint64_t)rl->length * vol->cluster_size;
		if (bm >= w->cbm_bytes) continue;
		if (bm + len > w->cbm_bytes) len = w->cbm_bytes - bm;
		w->bmr[w->nbmr].dev = (uint64_t)rl->lcn * vol->cluster_size; w->bmr[w->nbmr].len = len; w->bmr[w->nbmr].bm = bm; w->nbmr++;
		/* committed state = disk (the overlay holds only uncommitted changes) */
		if (nh_raw_pread(b, w->cbm + bm, len, (off_t)w->bmr[w->nbmr - 1].dev) != (ssize_t)len) return -1;
	}
	w->L.cs = vol->cluster_size; w->L.nclusters = (uint64_t)vol->nr_clusters;
	return 0;
}

static int direct_ok(struct nh_wal *w, uint64_t off, uint64_t n)
{
	if (!w->cbm || w->in_commit) return 0;
	uint64_t c0 = off / w->L.cs, c1 = (off + n - 1) / w->L.cs;
	if (c1 >= w->L.nclusters || c1 / 8 >= w->cbm_bytes) return 0;
	for (uint64_t c = c0; c <= c1; c++) if (w->cbm[c >> 3] & (1u << (c & 7))) return 0;
	for (uint64_t k = off / JB; k <= (off + n - 1) / JB; k++) if (hm_get(w->ovl, k) || hm_get(w->ucp, k)) return 0;
	return 1;
}

/* ---------------- read / write ---------------- */
s64 nh_wal_pread(nh_bwdev *b, void *buf, s64 n, s64 off)
{
	struct nh_wal *w = b->walp;
	s64 r = nh_raw_pread(b, buf, n, off);
	if (r < 0) return r;
	if (r < n) { memset((char *)buf + r, 0, n - r); }
	if (w->ovl->n) {
		for (uint64_t k = (uint64_t)off / JB; k <= (uint64_t)(off + n - 1) / JB; k++) {
			unsigned char **s = hm_get(w->ovl, k);
			if (!s) continue;
			uint64_t bs = k * JB, lo = bs > (uint64_t)off ? bs : (uint64_t)off;
			uint64_t hi = bs + JB < (uint64_t)(off + n) ? bs + JB : (uint64_t)(off + n);
			memcpy((char *)buf + (lo - off), *s + (lo - bs), hi - lo);
			r = n;
		}
	}
	return r;
}

s64 nh_wal_pwrite(nh_bwdev *b, const void *buf, s64 n, s64 off)
{
	struct nh_wal *w = b->walp;
	if (n <= 0) return 0;
	if (b->failed) { errno = b->failed; return -1; }
	if (direct_ok(w, off, n)) {
		s64 r = nh_raw_pwrite(b, buf, n, off);
		if (r > 0) { b->wal_direct_bytes += r; w->direct_dirty = 1; }
		return r;
	}
	for (uint64_t k = (uint64_t)off / JB; k <= (uint64_t)(off + n - 1) / JB; k++) {
		unsigned char **s = hm_put(w->ovl, k);
		if (!*s) {
			*s = malloc(JB);
			ssize_t rr = nh_raw_pread(b, *s, JB, (s64)(k * JB));
			if (rr < (ssize_t)JB) memset(*s + (rr > 0 ? rr : 0), 0, JB - (rr > 0 ? rr : 0));
			if (rr < 0 && b->failed) return -1;   /* the zero-filled block is never committed: fail-stop */
		}
		uint64_t bs = k * JB, lo = bs > (uint64_t)off ? bs : (uint64_t)off;
		uint64_t hi = bs + JB < (uint64_t)(off + n) ? bs + JB : (uint64_t)(off + n);
		memcpy(*s + (lo - bs), (const char *)buf + (lo - off), hi - lo);
		if (w->ovl->n >= w->ovl_cap) { b->wal_forced_commits++; if (nh_wal_commit(b, -1)) return -1; }
	}
	b->wal_logical_meta_bytes += n;
	return n;
}

/* ---------------- commit / checkpoint ---------------- */
static int checkpoint(nh_bwdev *b, int final)
{
	struct nh_wal *w = b->walp;
	(void)final;
	if (nh_raw_barrier(b)) return -1;        /* every in-place write so far is durable */
	{ vstamp st; vol_stamp(b, &st); if (st.ok) w->st = st; }   /* stamp of the committed state */
	w->start = w->tail; w->start_seq = w->seq; w->used = 0;
	hm_clear(w->ucp, 0);
	if (write_sb(b)) return -1;
	b->wal_checkpoints++;
	/* the new start must be durable before the next transaction overwrites the old
	 * circular area: a stale superblock would redo a cut-off prefix of old
	 * transactions over newer in-place blocks */
	if (nh_raw_barrier(b)) return -1;
	w->direct_dirty = 0;
	return 0;
}

/* marker < 0: no op marker */
int nh_wal_commit(nh_bwdev *b, long long marker)
{
	struct nh_wal *w = b->walp;
	if (w->ro) return 0;
	/* fail-stop: nothing reaches the disk any more, so no commit (empty or not) may succeed */
	if (b->failed) { errno = b->failed; return -1; }
	uint64_t n = w->ovl->n;
	if (!n) { if (marker >= 0 && nh_raw_mark(b, (uint64_t)marker)) return -1; return 0; }
	w->in_commit = 1;
	uint64_t ndesc = (n + PER_DESC - 1) / PER_DESC, total = ndesc + n + 1;
	if (total > w->logcap) { fprintf(stderr, "wal: transaction of %llu blocks exceeds the journal\n", (unsigned long long)total); w->in_commit = 0; errno = ENOSPC; return -1; }
	if (w->logcap - w->used < total && checkpoint(b, 0)) { w->in_commit = 0; return -1; }
	uint64_t *ks = hm_keys(w->ovl);
	unsigned char *t = calloc(ndesc + n, JB);
	for (uint64_t i = 0; i < ndesc; i++) {
		unsigned char *d = t + i * JB;
		uint64_t first = i * PER_DESC, cnt = n - first < PER_DESC ? n - first : PER_DESC;
		p32(d, JD_MAGIC); p32(d + 4, (uint32_t)i); p64(d + 8, w->seq);
		p32(d + 16, (uint32_t)ndesc); p32(d + 20, (uint32_t)n); p32(d + 24, (uint32_t)cnt);
		for (uint64_t j = 0; j < cnt; j++) p64(d + 32 + 8 * j, ks[first + j]);
	}
	for (uint64_t i = 0; i < n; i++) memcpy(t + (ndesc + i) * JB, *hm_get(w->ovl, ks[i]), JB);
	unsigned char c[JB]; memset(c, 0, JB);
	p32(c, JC_MAGIC); p64(c + 8, w->seq); p32(c + 16, (uint32_t)ndesc); p32(c + 20, (uint32_t)n);
	p32(c + 24, (uint32_t)crc32(0, t, (uInt)((ndesc + n) * JB)));
	p32(c + 28, (uint32_t)crc32(0, c, 28));
	int r = jwrite(b, t, ndesc + n, w->tail);
	/* B1: descriptor + images (+ direct data) durable. Adaptive mode skips it when no direct
	 * write happened since the last barrier: a commit record that lands before its images is
	 * rejected by the payload crc at replay, and the op was not acknowledged (B2 not passed). */
	if (!r) { if (b->one_barrier && !w->direct_dirty) b->wal_b1_skipped++; else r = nh_raw_barrier(b); }
	if (!r) r = jwrite(b, c, 1, w->tail + ndesc + n);       /* commit record */
	if (!r && marker >= 0) r = nh_raw_mark(b, (uint64_t)marker);
	if (!r) r = nh_raw_barrier(b);                         /* B2: commit durable before any in-place write */
	if (!r) w->direct_dirty = 0;
	if (!r) {
		/* in place, coalescing consecutive blocks */
		unsigned char *run = malloc(256 * JB);
		for (uint64_t i = 0; i < n && !r; ) {
			uint64_t j = i;
			while (j < n && j - i < 256 && ks[j] == ks[i] + (j - i)) { memcpy(run + (j - i) * JB, *hm_get(w->ovl, ks[j]), JB); j++; }
			if (nh_raw_pwrite(b, run, (j - i) * JB, (s64)(ks[i] * JB)) != (s64)((j - i) * JB)) r = -1;
			b->wal_inplace_bytes += (j - i) * JB;
			i = j;
		}
		free(run);
		/* committed bitmap shadow follows the committed blocks */
		if (w->cbm) for (uint64_t i = 0; i < n; i++) {
			uint64_t bs = ks[i] * JB;
			for (int q = 0; q < w->nbmr; q++) {
				uint64_t lo = bs > w->bmr[q].dev ? bs : w->bmr[q].dev;
				uint64_t hi = bs + JB < w->bmr[q].dev + w->bmr[q].len ? bs + JB : w->bmr[q].dev + w->bmr[q].len;
				if (lo < hi) memcpy(w->cbm + w->bmr[q].bm + (lo - w->bmr[q].dev), *hm_get(w->ovl, ks[i]) + (lo - bs), hi - lo);
			}
		}
		for (uint64_t i = 0; i < n; i++) *hm_put(w->ucp, ks[i]) = (unsigned char *)1;
		w->tail = (w->tail + total) % w->logcap; w->used += total; w->seq++;
		b->wal_commits++; b->wal_txn_blocks += n;
	}
	free(t); free(ks);
	/* the overlay is dropped either way; after a failed commit the library's cached state is
	 * ahead of the disk, so the volume must stop rather than carry on from it */
	if (r) nh_dev_fail(b, EIO);   /* no-op when a device error already set the cause */
	hm_clear(w->ovl, 1);
	w->in_commit = 0;
	return r;
}

/* commit what is pending, then checkpoint: the journal is empty and the stamp is current */
int nh_wal_checkpoint(nh_bwdev *b)
{
	struct nh_wal *w = b->walp;
	if (!w || w->ro) return 0;
	if (nh_wal_commit(b, -1)) return -1;
	if (checkpoint(b, 0)) { nh_dev_fail(b, EIO); return -1; }
	return 0;
}

int nh_wal_set_owner_dirty(nh_bwdev *b, int v)
{
	struct nh_wal *w = b->walp;
	if (!w || w->ro) return 0;
	w->owner_dirty = (uint32_t)v;
	return write_sb(b);   /* made durable by the next commit's first barrier */
}

int nh_wal_close(nh_bwdev *b)
{
	struct nh_wal *w = b->walp;
	if (!w) return 0;
	int r = 0;
	if (!w->ro) {
		r = nh_wal_commit(b, -1);
		if (!r) r = checkpoint(b, 1);
	}
	hm_free(w->ovl, 1); hm_free(w->ucp, 0);
	free(w->cbm); free(w->bmr); free(w);
	b->walp = NULL; b->wal = 0;
	return r;
}

/* ---------------- commands ---------------- */
/*
 * Creates /$BwJournal (contiguous, hidden + system, zero-filled) and writes its superblock.
 * replace: an existing /$BwJournal (truncated, fragmented or with a broken superblock) is
 * removed first. The volume is mounted read-write WITHOUT the journal for this: the creation
 * itself is not crash-safe (as with jinit), which is why it runs only when the journal is
 * unusable and the volume is clean (the read-only guard already ran).
 * Returns 0, or -errno with a message on stderr.
 */
int nh_journal_create(const nh_opts *oin, long mib, int replace)
{
	nh_opts o = *oin;
	o.rw = 1; o.wal = 0; o.dirty_guard = 0; o.ro_guard = 0; o.wlog = NULL;
	int saved_forced = nh_guard_forced_ro;
	int err; ntfs_volume *vol = nh_mount(&o, 0, &err);
	nh_guard_forced_ro = saved_forced;
	if (!vol) { fprintf(stderr, "journal: mount failed: %s\n", strerror(err)); return -err; }
	if (NVolReadOnly(vol)) { fprintf(stderr, "journal: refused: read-only (EROFS)\n"); nh_umount(vol); return -EROFS; }
	ntfs_inode *ni; ntfs_attr *na;
	int r = 0;
	if (replace) {
		ni = nh_lookup(vol, "/" JNAME);
		if (ni) {
			ntfs_inode_close(ni);
			r = nh_unlink(vol, "/" JNAME);
			if (r) { fprintf(stderr, "journal: remove old /%s: %s\n", JNAME, strerror(-r)); nh_umount(vol); return r; }
			fprintf(stderr, "journal: removed the unusable /%s\n", JNAME);
		}
	}
	r = nh_open_data(vol, "/" JNAME, NULL, 1, &ni, &na);
	if (r) { fprintf(stderr, "journal: create: %s\n", strerror(-r)); nh_umount(vol); return r; }
	unsigned char *z = calloc(1, 1 << 20);
	for (long i = 0; i < mib && !r; i++) if (nh_pwrite_all(na, (s64)i << 20, 1 << 20, z) != 1 << 20) r = -(errno ? errno : EIO);
	free(z);
	int64_t lcn = -1, len = 0, runs = 0;
	if (!r && !ntfs_attr_map_whole_runlist(na)) for (runlist_element *rl = na->rl; rl && rl->length; rl++) { runs++; lcn = rl->lcn; len = rl->length; }
	int r2 = nh_close_data(ni, na, 1);
	ni = nh_lookup(vol, "/" JNAME);
	if (ni) { le32 at = ni->flags | FILE_ATTR_HIDDEN | FILE_ATTR_SYSTEM; ntfs_set_ntfs_attrib(ni, (const char *)&at, sizeof at, 0); ntfs_inode_close(ni); }
	uint32_t cs = vol->cluster_size;
	if (r || r2) {
		/* out of space (or an I/O error) part-way: do not leave a partial file holding the space */
		fprintf(stderr, "journal: write failed (%s); removed the partial /%s\n", strerror(-(r ? r : r2)), JNAME);
		nh_unlink(vol, "/" JNAME);
		nh_umount(vol);
		return r ? r : r2;
	}
	if (runs != 1 || lcn <= 0 || (uint64_t)len * cs < (uint64_t)mib << 20) {
		/* free space too fragmented: remove the file again rather than leave a useless one */
		fprintf(stderr, "journal: /%s is not contiguous (runs=%lld); removed\n", JNAME, (long long)runs);
		nh_unlink(vol, "/" JNAME);
		nh_umount(vol);
		return -ENOSPC;
	}
	if (nh_umount(vol)) { fprintf(stderr, "journal: unmount after create failed\n"); return -EIO; }
	nh_bwdev bb;
	int oe = nh_raw_open(&bb, o.image, o.io, O_RDWR);
	if (oe) { fprintf(stderr, "journal: reopen %s: %s\n", o.image, strerror(-oe)); return oe; }
	jloc L;
	if (locate_journal(&bb, &L) || L.jdev != (uint64_t)lcn * cs) { fprintf(stderr, "journal: raw scan did not find /%s\n", JNAME); nh_raw_close(&bb); return -EIO; }
	jsb s = { (uint64_t)mib << 20 >> 12, 0, 1, L.serial, 0, 2, { 0 } };
	vol_stamp(&bb, &s.st);
	uint8_t p[512]; sb_enc(p, &s);
	if (nh_raw_pwrite(&bb, p, 512, (s64)L.jdev) != 512 || nh_raw_barrier(&bb)) { int e = errno ? errno : EIO; perror("journal superblock"); nh_raw_close(&bb); return -e; }
	nh_raw_close(&bb);
	printf("jinit journal=/%s mft_record=%llu lcn=%lld bytes=%llu blocks=%llu stamp=%d\n", JNAME, (unsigned long long)L.mft_no,
	       (long long)lcn, (unsigned long long)L.jbytes, (unsigned long long)s.jblocks, s.st.ok);
	return 0;
}

/* jinit --image IMG [--size MiB] [--replace]: creates /$BwJournal (contiguous) + superblock */
int cmd_jinit(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	long mib = 32; int replace = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--size") && i + 1 < argc) mib = atol(argv[i + 1]);
		if (!strcmp(argv[i], "--replace")) replace = 1;
	}
	if (mib < 1 || mib > 1024) { fprintf(stderr, "jinit: --size 1..1024 MiB\n"); return 2; }
	if (!o.image) return 2;
	int r = nh_journal_create(&o, mib, replace);
	return r == -EROFS ? 30 : r ? 1 : 0;
}

/* jinfo IMG: raw, read-only: superblock + pending valid transactions */
int cmd_jinfo(int argc, char **argv)
{
	const char *img = argc > 1 ? argv[1] : NULL;
	if (!img || (!nh_path_allowed(img) && !nh_dev_allowed(img))) { fprintf(stderr, "jinfo: path must be under %s\n", NH_ALLOWED_ROOT); return 2; }
	int fd = nh_open_target(img, O_RDONLY);
	if (fd < 0) { perror(img); return 1; }
	nh_bwdev b; memset(&b, 0, sizeof b); b.fd = fd; b.logfd = -1; b.wal_req = 1; b.isdev = nh_dev_allowed(img);
	int r = nh_wal_open(&b, 1);
	if (r || !b.walp) { printf("jinfo journal=none\n"); close(fd); return 1; }
	struct nh_wal *w = b.walp;
	printf("jinfo journal=/%s mft_record=%llu jdev=%llu blocks=%llu start=%llu start_seq=%llu owner_dirty=%u pending_txns=%llu pending_blocks=%llu stale=%d stale_txns=%llu vol_lsn=%llu lf_crc=%08x\n",
	       JNAME, (unsigned long long)w->L.mft_no, (unsigned long long)w->L.jdev, (unsigned long long)w->logcap + 1,
	       (unsigned long long)w->start, (unsigned long long)w->start_seq, w->owner_dirty,
	       (unsigned long long)b.wal_replayed_txns, (unsigned long long)b.wal_replayed_blocks, b.wal_stale,
	       (unsigned long long)b.wal_stale_txns, (unsigned long long)w->st.vol_lsn, w->st.lf_crc);
	hm_free(w->ovl, 1); hm_free(w->ucp, 0); free(w);
	close(fd);
	return 0;
}

/*
 * recover --image IMG [--clear-own-dirty]: replays the journal in place, no libntfs
 * mount. --clear-own-dirty then clears VOLUME_IS_DIRTY when the superblock says our
 * own mount set it (the journal brought the volume back to a committed state).
 */
int cmd_recover(int argc, char **argv)
{
	nh_opts o; nh_opts_parse(&o, &argc, &argv);
	int clear = o.clear_own_dirty;   /* nh_opts_parse consumes the flag */
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--clear-own-dirty")) clear = 1;
	if (!o.image || (!nh_path_allowed(o.image) && !nh_dev_allowed(o.image))) { fprintf(stderr, "recover: image must be under %s\n", NH_ALLOWED_ROOT); return 2; }
	int fd = nh_open_target(o.image, O_RDWR);
	if (fd < 0) { perror(o.image); return 1; }
	nh_bwdev b; memset(&b, 0, sizeof b); b.fd = fd; b.logfd = -1; b.wal_req = 1; b.fullsync = 1; b.isdev = nh_dev_allowed(o.image);
	double t = nh_now();
	int r = nh_wal_open(&b, 0);
	if (r || !b.walp) { printf("recover journal=none\n"); close(fd); return 1; }
	int own = (int)b.walp->owner_dirty;
	printf("recover replayed_txns=%llu replayed_blocks=%llu stale=%d stale_discarded_txns=%llu owner_dirty=%d ms=%.1f\n", (unsigned long long)b.wal_replayed_txns,
	       (unsigned long long)b.wal_replayed_blocks, b.wal_stale, (unsigned long long)b.wal_stale_txns, own, (nh_now() - t) * 1000);
	hm_free(b.walp->ovl, 1); hm_free(b.walp->ucp, 0); free(b.walp);
	nh_raw_barrier(&b);
	close(fd);
	if (clear && own) {
		o.rw = 1; o.ro_guard = 0; o.dirty_guard = 0; o.wal = 1;
		int err; ntfs_volume *vol = nh_mount(&o, 0, &err);
		if (!vol) { fprintf(stderr, "recover: mount for clear failed: %s\n", strerror(err)); return 3; }
		int cr = ntfs_volume_write_flags(vol, vol->flags & ~VOLUME_IS_DIRTY);
		nh_wal_set_owner_dirty(nh_dev_of(vol), 0);
		if (nh_umount(vol) || cr) { fprintf(stderr, "recover: clear failed\n"); return 1; }
		printf("recover cleared_own_dirty=1\n");
	}
	return 0;
}

/* 1 when the overlay holds an uncommitted block inside [off, off+len): kernel-offloaded I/O on
 * that range must wait for a commit (it goes straight to the device, past the overlay) */
int nh_wal_range_pending(nh_bwdev *b, uint64_t off, uint64_t len)
{
	struct nh_wal *w = b->walp;
	if (!w || !w->ovl || !w->ovl->n || !len) return 0;
	uint64_t k0 = off / JB, k1 = (off + len - 1) / JB;
	if (k1 - k0 + 1 <= w->ovl->n) {
		for (uint64_t k = k0; k <= k1; k++) if (hm_get(w->ovl, k)) return 1;
		return 0;
	}
	for (size_t i = 0; i < w->ovl->cap; i++) {
		uint64_t k = w->ovl->k[i];
		if (k && k - 1 >= k0 && k - 1 <= k1) return 1;
	}
	return 0;
}

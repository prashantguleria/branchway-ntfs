#!/usr/bin/env python3
"""NTFS-2 engine tests (deliverable B), file images only. No device is opened, nothing is mounted.

Cases:
  stamp        jinit writes a v2 superblock with a volume stamp; a journaled bench passes fsck and verify;
               every B/D/C manifest line has a T (timestamp) line.
  replay       a crash image (device pulled mid-run) with pending transactions: the stamp matches, so
               the transactions are replayed (no false stale); fsck + verify up to the last C pass.
  stale-lsn    the crash image is cloned; on the clone the journal is replayed, then "Windows" touches
               the volume (the $Volume record LSN moves, as chkdsk does when it clears the dirty bit)
               and our engine writes more files; then the old journal (with its pending transactions)
               is copied back. Expected: open says stale, discards the transactions, fsck passes and
               every file written after the crash is intact.
  stale-logfile the same with a change inside the $LogFile restart pages instead (raw recover only:
               libntfs-3g would refuse a hand-made restart page), then the bytes are put back.
  limit-own    the documented limit: our own engine's writes move neither stamp field, so an old
               journal copied back over them IS replayed. Reported, not a pass/fail check.
  missing      /$BwJournal removed (what chkdsk does): a read-write --wal mount re-creates it.
  truncated    /$BwJournal truncated to 1 MiB: re-created (old file removed first).
  badsb        the journal superblock zeroed: re-created.
  ro-missing   journal missing, read-only mount with --wal: mounts, nothing written, no refusal.
  nospace      journal missing and no room for a new one: mounts read-only (never refuses).
usage: ntfs2_test.py [--work DIR] [--only a,b]   (DIR under /Volumes/Media/spikes/ntfs/, default s6work/ntfs2/engine)"""
import hashlib, json, os, re, shutil, struct, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 's6'))
from s6lib import B, H, MKNTFS, sh, kv, last

ARGS = sys.argv[1:]
WORK = ARGS[ARGS.index('--work') + 1] if '--work' in ARGS else B + '/s6work/ntfs2/engine'
ONLY = ARGS[ARGS.index('--only') + 1].split(',') if '--only' in ARGS else None
SIZE = 512 << 20

def mkimg(path, size=SIZE, journal=True):
    if os.path.exists(path): os.unlink(path)
    with open(path, 'wb') as f: f.truncate(size)
    rc, o = sh([MKNTFS, '-Q', '-F', '-c', '4096', '-s', '512', '-p', '2048', '-H', '255', '-S', '63', '-L', 'NTFS2', path, str(size // 512)])
    assert rc == 0, o[-300:]
    if journal:
        rc, o = sh([H, 'jinit', '--image', path, '--size', '32']); assert rc == 0, o[-300:]
        return o
    return ''

def clone(a, b):
    if os.path.exists(b): os.unlink(b)
    rc, o = sh(['cp', '-c', a, b]); assert rc == 0, o

def jinfo(img):
    rc, o = sh([H, 'jinfo', img]); return rc, kv(last(o, 'jinfo ')), o

def fsck(img):
    rc, o = sh([H, 'fsck', '--image', img]); return kv(last(o, 'fsck ')).get('result'), o

def verify(img, man, comp=None, root='/w'):
    cmd = [H, 'verify', '--image', img, '--manifest', man, '--root', root]
    if comp is not None: cmd += ['--completed-op', str(comp)]
    rc, o = sh(cmd); return kv(last(o, 'verify ')), o

def bench(img, man, workload, count, commit='op', extra=(), env=None, d='/w'):
    open(man, 'a').close()
    rc, o = sh([H, 'bench', '--image', img, '--rw', '--wal', '--quiet', '--commit', commit, '--workload', workload, '--count', str(count), '--manifest', man, '--dir', d] + list(extra), env=env, timeout=900)
    return rc, o

def tags(man):
    t = {'B': [], 'D': [], 'C': [], 'T': []}
    for l in open(man):
        f = l.split()
        if f and f[0] in t: t[f[0]].append((int(f[1]),) + tuple(f[2:]) if f[0] == 'T' else int(f[1]))
    return t

# ---- raw NTFS helpers for the "Windows touched it" simulation ----
def geom(img):
    with open(img, 'rb') as f: bs = f.read(512)
    bps = struct.unpack_from('<H', bs, 0x0B)[0]; spc = bs[0x0D]; cs = bps * spc
    mft = struct.unpack_from('<q', bs, 0x30)[0]; mirr = struct.unpack_from('<q', bs, 0x38)[0]; cpm = struct.unpack_from('<b', bs, 0x40)[0]
    rs = cpm * cs if cpm > 0 else 1 << (-cpm)
    return cs, mft * cs, rs, mirr * cs

def bump_volume_lsn(img, delta=0x10000):
    # MFT record 3 ($Volume), header LSN (first sector: no fixup slot). $MFTMirr holds records 0-3 and
    # libntfs-3g refuses a mount when they differ, so the mirror copy moves too (as Windows does).
    cs, mftoff, rs, mirroff = geom(img)
    with open(img, 'r+b') as f:
        f.seek(mftoff + 3 * rs + 8); v = struct.unpack('<Q', f.read(8))[0]
        for base in (mftoff, mirroff):
            f.seek(base + 3 * rs + 8); f.write(struct.pack('<Q', v + delta))
        f.flush(); os.fsync(f.fileno())
    return v, v + delta

def logfile_offset(img):
    cs, mftoff, rs, _ = geom(img)
    with open(img, 'rb') as f: f.seek(mftoff + 2 * rs); r = bytearray(f.read(rs))
    # fixups
    uo, uc = struct.unpack_from('<HH', r, 4); usa = r[uo:uo + 2 * uc]
    for i in range(1, uc): r[i * 512 - 2:i * 512] = usa[2 * i:2 * i + 2]
    a = struct.unpack_from('<H', r, 0x14)[0]
    while True:
        t, l = struct.unpack_from('<II', r, a)
        if t == 0xFFFFFFFF: raise RuntimeError('no $DATA in $LogFile')
        if t == 0x80 and r[a + 9] == 0: break
        a += l
    p = a + struct.unpack_from('<H', r, a + 0x20)[0]
    hdr = r[p]; lb, ob = hdr & 15, hdr >> 4
    lcn = int.from_bytes(r[p + 1 + lb:p + 1 + lb + ob], 'little', signed=True)
    return lcn * cs

def journal_region(img):
    rc, j, o = jinfo(img); assert rc == 0, o
    return int(j['jdev']), int(j['blocks']) * 4096

def read_region(img, off, n):
    with open(img, 'rb') as f: f.seek(off); return f.read(n)

def write_region(img, off, data):
    with open(img, 'r+b') as f: f.seek(off); f.write(data); f.flush(); os.fsync(f.fileno())

def sha_files(img, paths):
    out = {}
    for p in paths:
        rc, o = sh([H, 'sha', '--image', img, p]); out[p] = last(o, '').strip() if rc == 0 else 'ERR'
    return out

# ---- crash image with pending journal transactions ----
def crash_image(d):
    """512 MiB image, journaled bench (op commit) stopped by the fault hook mid-run: pending transactions"""
    img = d + '/crash.img'; mkimg(img)
    man0 = d + '/base.man'; open(man0, 'w').close()
    rc, o = bench(img, man0, 'small', 200); assert rc == 0, o[-300:]
    nw = int(kv(last(o, 'bench '))['dev_writes'])
    mkimg(img); man = d + '/crash.man'; open(man, 'w').close()
    rc, o = bench(img, man, 'small', 200, env={'NH_FAULT_ENXIO_AFTER_WRITES': str(nw // 2)})
    assert 'injected ENXIO' in o, o[-500:]
    comp = max(tags(man)['C'], default=0)
    return img, man, comp

R = []
def record(name, checks, info):
    ok = all(checks.values())
    R.append({'case': name, 'pass': ok, 'checks': checks, 'info': info})
    print(f"{name}: {'PASS' if ok else 'FAIL ' + str([k for k, v in checks.items() if not v])} {json.dumps(info)[:400]}", flush=True)

def c_stamp():
    d = WORK + '/stamp'; os.makedirs(d, exist_ok=True)
    img = d + '/s.img'; o = mkimg(img)
    rc, j, _ = jinfo(img)
    man = d + '/s.man'; open(man, 'w').close()
    brc, bo = bench(img, man, 'small', 100, commit='ms=200')
    res, _ = fsck(img); v, _ = verify(img, man)
    t = tags(man)
    tset = {(x[0], x[1]) for x in t['T']}
    need = {(n, 'B') for n in t['B']} | {(n, 'D') for n in t['D']} | {(n, 'C') for n in t['C']}
    times = [float(x[2]) for x in t['T']]
    record('stamp', {'jinit_stamp': 'stamp=1' in o, 'jinfo_ok': rc == 0 and j.get('stale') == '0' and j.get('pending_txns') == '0',
                     'bench_ok': brc == 0, 'fsck_pass': res == 'PASS', 'verify_pass': v.get('result') == 'PASS',
                     'T_for_every_BDC': need <= tset and len(need) > 0, 'T_monotonic': times == sorted(times)},
           {'vol_lsn': j.get('vol_lsn'), 'lf_crc': j.get('lf_crc'), 'nB': len(t['B']), 'nC': len(t['C']), 'nT': len(t['T']), 'verify_checked': v.get('checked')})

def c_replay():
    d = WORK + '/replay'; os.makedirs(d, exist_ok=True)
    img, man, comp = crash_image(d)
    rc, j, _ = jinfo(img)
    rrc, ro = sh([H, 'recover', '--image', img]); r = kv(last(ro, 'recover '))
    res, _ = fsck(img); v, _ = verify(img, man, comp)
    record('replay', {'pending_before': int(j.get('pending_txns', 0)) > 0, 'not_stale': j.get('stale') == '0' and r.get('stale') == '0',
                      'replayed': int(r.get('replayed_txns', 0)) == int(j.get('pending_txns', -1)), 'fsck_pass': res == 'PASS',
                      'verify_pass_upto_last_C': v.get('result') == 'PASS'},
           {'pending_txns': j.get('pending_txns'), 'replayed_txns': r.get('replayed_txns'), 'last_C': comp, 'verify_checked': v.get('checked')})

def stale_setup(d, windows):
    """crash image X; clone Y; Y: replay, optional Windows touch, our engine writes /after; old journal back"""
    x, man, comp = crash_image(d)
    joff, jlen = journal_region(x)
    old = read_region(x, joff, jlen)
    _, jx, _ = jinfo(x)
    y = d + '/clone.img'; clone(x, y)
    # replay, and clear the dirty bit our crashed mount left (Windows' chkdsk clears it as well)
    rc, o = sh([H, 'recover', '--image', y, '--clear-own-dirty']); assert rc == 0, o[-300:]
    info = {'pending_in_old_journal': jx.get('pending_txns')}
    if windows == 'lsn': info['volume_lsn'] = bump_volume_lsn(y)
    man2 = d + '/after.man'; open(man2, 'w').close()
    brc, bo = bench(y, man2, 'small', 60, d='/after')
    info['after_bench_rc'] = brc
    write_region(y, joff, old)          # the old journal (old superblock + pending transactions) is back
    return y, man2, info

def c_stale_lsn():
    d = WORK + '/stale-lsn'; os.makedirs(d, exist_ok=True)
    y, man2, info = stale_setup(d, 'lsn')
    rc, j, _ = jinfo(y)
    rrc, ro = sh([H, 'recover', '--image', y]); r = kv(last(ro, 'recover '))
    res, fo = fsck(y); v, _ = verify(y, man2, root='/after')
    m = re.search(r'wal: journal is stale[^\n]*', ro)
    record('stale-lsn', {'after_bench_ok': info['after_bench_rc'] == 0, 'jinfo_stale': j.get('stale') == '1' and int(j.get('stale_txns', 0)) > 0,
                         'recover_discards': r.get('stale') == '1' and r.get('replayed_txns') == '0' and int(r.get('stale_discarded_txns', 0)) > 0,
                         'logged_why': m is not None, 'fsck_pass': res == 'PASS', 'after_files_intact': v.get('result') == 'PASS' and int(v.get('checked', 0)) > 0},
           dict(info, discarded=r.get('stale_discarded_txns'), log=m.group(0)[:200] if m else None, verify_checked=v.get('checked')))
    # a further journaled mount works and the journal is clean again
    man3 = d + '/again.man'; open(man3, 'w').close()
    brc, _ = bench(y, man3, 'small', 20, d='/again'); res2, _ = fsck(y); _, j2, _ = jinfo(y)
    record('stale-lsn-remount', {'bench_ok': brc == 0, 'fsck_pass': res2 == 'PASS', 'journal_clean': j2.get('stale') == '0' and j2.get('pending_txns') == '0'}, {})

def c_stale_logfile():
    d = WORK + '/stale-logfile'; os.makedirs(d, exist_ok=True)
    x, man, comp = crash_image(d)
    lo = logfile_offset(x)
    orig = read_region(x, lo, 8192)
    write_region(x, lo + 4096 + 64, b'WINDOWS!')     # inside the second restart page
    rc, j, _ = jinfo(x)
    rrc, ro = sh([H, 'recover', '--image', x]); r = kv(last(ro, 'recover '))
    write_region(x, lo, orig)                          # put the bytes back for fsck
    res, _ = fsck(x)
    record('stale-logfile', {'jinfo_stale': j.get('stale') == '1', 'recover_discards': r.get('stale') == '1' and r.get('replayed_txns') == '0',
                             'fsck_runs': res in ('PASS', 'FAIL')},
           {'pending': j.get('pending_txns'), 'discarded': r.get('stale_discarded_txns'), 'fsck_after_discard': res,
            'note': 'discarding the tail of a real crash leaves the un-replayed state; fsck result is informational here'})

def c_limit_own():
    d = WORK + '/limit-own'; os.makedirs(d, exist_ok=True)
    y, man2, info = stale_setup(d, None)
    rc, j, _ = jinfo(y)
    rrc, ro = sh([H, 'recover', '--image', y]); r = kv(last(ro, 'recover '))
    res, _ = fsck(y); v, _ = verify(y, man2, root='/after')
    R.append({'case': 'limit-own', 'pass': None, 'info': dict(info, stale=j.get('stale'), replayed=r.get('replayed_txns'), fsck=res, after_verify=v.get('result'))})
    print(f"limit-own (documented limit, not a check): stale={j.get('stale')} replayed={r.get('replayed_txns')} fsck={res} after_verify={v.get('result')}", flush=True)

def recreate_case(name, damage):
    d = WORK + '/' + name; os.makedirs(d, exist_ok=True)
    img = d + '/r.img'; mkimg(img)
    man0 = d + '/pre.man'; open(man0, 'w').close()
    brc0, _ = bench(img, man0, 'small', 30, d='/pre')
    damage(img)
    rc0, j0, _ = jinfo(img)
    man = d + '/r.man'; open(man, 'w').close()
    brc, bo = bench(img, man, 'small', 50)
    rc, j, _ = jinfo(img)
    res, _ = fsck(img); v, _ = verify(img, man); v0, _ = verify(img, man0, root='/pre')
    record(name, {'journal_unusable_before': rc0 != 0, 'recreated_logged': 're-creating /$BwJournal' in bo, 'bench_ok': brc == 0,
                  'journal_usable_after': rc == 0 and j.get('blocks') == '8192', 'fsck_pass': res == 'PASS',
                  'verify_pass': v.get('result') == 'PASS', 'older_files_intact': v0.get('result') == 'PASS'},
           {'msg': [l for l in bo.splitlines() if l.startswith(('wal:', 'journal:'))][:4]})

def dmg_missing(img):
    rc, o = sh([H, 'rm', '--image', img, '--rw', '--no-wal', '/$BwJournal']); assert rc == 0, o
def dmg_trunc(img):
    rc, o = sh([H, 'truncate', '--image', img, '--rw', '--no-wal', '/$BwJournal', str(1 << 20)]); assert rc == 0, o
def dmg_sb(img):
    joff, _ = journal_region(img); write_region(img, joff, bytes(512))

def c_ro_missing():
    d = WORK + '/ro-missing'; os.makedirs(d, exist_ok=True)
    img = d + '/m.img'; mkimg(img); dmg_missing(img)
    before = hashlib.sha256(open(img, 'rb').read()).hexdigest()
    rc, o = sh([H, 'ls', '--image', img, '--wal', '/'])
    after = hashlib.sha256(open(img, 'rb').read()).hexdigest()
    record('ro-missing', {'mounts': rc == 0, 'nothing_written': before == after}, {'rc': rc})

def c_nospace():
    d = WORK + '/nospace'; os.makedirs(d, exist_ok=True)
    img = d + '/n.img'; mkimg(img, size=128 << 20); dmg_missing(img)
    fill = d + '/fill.bin'
    with open(fill, 'wb') as f: f.write(os.urandom(1 << 20) * 120)
    sh([H, 'write', '--image', img, '--rw', '--no-wal', fill, '/fill.bin'])   # ENOSPC part-way is the point
    os.unlink(fill)
    man = d + '/n.man'; open(man, 'w').close()
    brc, bo = bench(img, man, 'small', 5)
    res, _ = fsck(img)
    rc2, j2, _ = jinfo(img)
    record('nospace', {'not_refused_mounted_ro': brc == 30 and 'mounting read-only' in bo, 'partial_journal_removed': rc2 != 0 and 'removed the partial' in bo,
                        'fsck_pass': res == 'PASS'},
           {'rc': brc, 'msg': [l for l in bo.splitlines() if l.startswith(('wal:', 'journal:'))][:4]})

CASES = [('stamp', c_stamp), ('replay', c_replay), ('stale-lsn', c_stale_lsn), ('stale-logfile', c_stale_logfile), ('limit-own', c_limit_own),
         ('missing', lambda: recreate_case('missing', dmg_missing)), ('truncated', lambda: recreate_case('truncated', dmg_trunc)),
         ('badsb', lambda: recreate_case('badsb', dmg_sb)), ('ro-missing', c_ro_missing), ('nospace', c_nospace)]

def main():
    os.makedirs(WORK, exist_ok=True)
    assert os.path.realpath(WORK).startswith(B + '/'), 'work dir must be under ' + B
    t0 = time.time()
    for n, f in CASES:
        if ONLY and n not in ONLY: continue
        try: f()
        except Exception as e:
            R.append({'case': n, 'pass': False, 'error': repr(e)[:500]}); print(f'{n}: ERROR {e!r}'[:600], flush=True)
    checked = [r for r in R if r['pass'] is not None]
    ok = sum(1 for r in checked if r['pass'])
    json.dump(R, open(WORK + '/ntfs2_test.json', 'w'), indent=1)
    print(f'ntfs2_test {ok}/{len(checked)} PASS ({time.time() - t0:.0f}s)')
    sys.exit(0 if ok == len(checked) else 1)

if __name__ == '__main__':
    main()

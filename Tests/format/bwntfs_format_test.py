#!/usr/bin/env python3
"""bwntfs-format tests. Image files only: every /dev path used here is refused by name (or does not
exist) before the tool opens anything, and those cases also pass --dry-run, so no path can reach mkntfs.

Run: make test-format   (needs build/bwntfs-format, build/ntfsharness, build/libntfs-3g/sbin/mkntfs)
"""
import os, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(REPO, 'build', 'bwntfs-format')
H = os.path.join(REPO, 'build', 'ntfsharness')
DIR = '/Volumes/Media/spikes/ntfs/s6work/ntfs2/format'
results = []

def run(*args):
    p = subprocess.run([TOOL] + list(args), capture_output=True, text=True, timeout=300)
    return p.returncode, p.stdout + p.stderr

def image(name, mib=64):
    p = os.path.join(DIR, name)
    if os.path.exists(p): os.remove(p)
    with open(p, 'wb') as f: f.truncate(mib << 20)
    return os.path.realpath(p)

def untouched(p):
    with open(p, 'rb') as f: return f.read(1 << 20) == b'\0' * (1 << 20)

def case(name, ok, detail=''):
    results.append((name, ok)); print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else ' :: ' + detail[-600:]))

def main():
    os.makedirs(DIR, exist_ok=True)
    # 1. a fresh image: mkntfs + journal, fsck clean, label and journal present
    p = image('ok.img')
    rc, out = run('-L', 'FMTTEST', '--journal-mib', '8', p)
    fs = subprocess.run([H, 'fsck', '--image', p], capture_output=True, text=True).stdout
    pr = subprocess.run([H, 'statfs', '--image', p], capture_output=True, text=True).stdout
    ji = subprocess.run([H, 'jinfo', p], capture_output=True, text=True).stdout
    case('formats an image with a journal', rc == 0 and 'result=PASS' in fs and 'label=FMTTEST' in pr and 'journal=/$BwJournal' in ji
         and 'blocks=2048' in ji, out + fs + pr + ji)
    # 2. a process holds the image: refused as busy, nothing written
    p = image('busy.img')
    holder = subprocess.Popen([sys.executable, '-I', '-c', 'import sys,time; f=open(sys.argv[1],"rb"); time.sleep(60)', p])
    try:
        time.sleep(0.5)
        rc, out = run('-L', 'BUSY', '--journal-mib', '8', p)
    finally:
        holder.kill(); holder.wait()
    case('refuses an image another process holds', rc == 4 and 'open in another process' in out and untouched(p), out)
    # 3. once released it formats
    rc, out = run('-L', 'BUSY', '--journal-mib', '8', p)
    case('formats it once released', rc == 0, out)
    # 4. symbolic link to an image
    p = image('target.img'); link = os.path.join(DIR, 'link.img')
    if os.path.lexists(link): os.remove(link)
    os.symlink(p, link)
    rc, out = run('-L', 'X', link)
    case('refuses a symbolic link', rc == 3 and 'symbolic link' in out and untouched(p), out)
    # 5. relative path
    rc, out = subprocess.run([TOOL, '-L', 'X', 'target.img'], cwd=DIR, capture_output=True, text=True).returncode, ''
    case('refuses a relative path', rc == 3 and untouched(p), out)
    # 6. a directory
    rc, out = run('-L', 'X', DIR)
    case('refuses a directory', rc == 3 and 'not a regular image file' in out, out)
    # 7. whole-disk device nodes are refused by name (dry run: nothing could be written)
    for node in ('/dev/disk0', '/dev/rdisk0', '/dev/disk4'):
        rc, out = run('--dry-run', '-L', 'X', node)
        case('refuses whole disk ' + node, rc == 3 and ('not a partition node' in out or 'no such' in out), out)
    # 8. APFS volume nodes (diskNsMsK) and other /dev names
    for node in ('/dev/disk3s1s1', '/dev/null', '/dev/zero'):
        rc, out = run('--dry-run', '-L', 'X', node)
        case('refuses ' + node, rc == 3, out)
    # 9. a partition node that does not exist
    rc, out = run('--dry-run', '-L', 'X', '/dev/disk999s1')
    case('refuses a missing partition node', rc == 3 and 'no such' in out, out)
    # 10. too small, journal too large, no label, label too long, two targets
    small = os.path.join(DIR, 'small.img')
    with open(small, 'wb') as f: f.truncate(1 << 20)
    rc, out = run('-L', 'X', os.path.realpath(small)); case('refuses an image under 16 MiB', rc == 3, out)
    p = image('j.img', 32)
    rc, out = run('-L', 'X', '--journal-mib', '16', p); case('refuses a journal over a quarter of the volume', rc == 3 and untouched(p), out)
    rc, out = run(p); case('needs a label', rc == 2 and untouched(p), out)
    rc, out = run('-L', 'L' * 129, p); case('refuses a label over 128 characters', rc == 2, out)
    rc, out = run('-L', 'X', p, p); case('takes exactly one target', rc == 2, out)
    # 11. dry run on an image: plan printed, nothing written
    rc, out = run('--dry-run', '-L', 'DRY', '--journal-mib', '4', p)
    case('dry run writes nothing', rc == 0 and 'kind=image' in out and untouched(p), out)
    # 12. formatting an image that already holds NTFS replaces it (fresh volume, new label, fsck clean)
    p = os.path.join(DIR, 'ok.img')
    rc, out = run('-L', 'AGAIN', '--journal-mib', '4', os.path.realpath(p))
    pr = subprocess.run([H, 'statfs', '--image', p], capture_output=True, text=True).stdout
    case('reformats an existing NTFS image', rc == 0 and 'label=AGAIN' in pr, out + pr)
    n = sum(1 for _, ok in results if ok)
    print(f'bwntfs-format: {n}/{len(results)} passed')
    return 0 if n == len(results) else 1

if __name__ == '__main__':
    sys.exit(main())

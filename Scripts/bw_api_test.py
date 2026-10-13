#!/usr/bin/env python3
"""Runs build/bw_api_test (the module engine API) on a fresh journaled 512 MiB image.
Images only, under /Volumes/Media/spikes/ntfs/s6work/ntfs2/api (or --work)."""
import os, sys, subprocess
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 's6'))
from s6lib import H, MKNTFS, REPO, sh

ARGS = sys.argv[1:]
WORK = ARGS[ARGS.index('--work') + 1] if '--work' in ARGS else '/Volumes/Media/spikes/ntfs/s6work/ntfs2/api'
os.makedirs(WORK, exist_ok=True)
img = WORK + '/base.img'
if os.path.exists(img): os.unlink(img)
size = 512 << 20
with open(img, 'wb') as f: f.truncate(size)
rc, o = sh([MKNTFS, '-Q', '-F', '-c', '4096', '-s', '512', '-p', '2048', '-H', '255', '-S', '63', '-L', 'NTFS2', img, str(size // 512)])
assert rc == 0, o[-300:]
rc, o = sh([H, 'jinit', '--image', img, '--size', '32']); assert rc == 0, o[-300:]
cmd = [REPO + '/build/bw_api_test', img, WORK] + [a for a in ARGS if not a.startswith('--') and a != WORK]
r = subprocess.run(cmd, env=dict(os.environ, BW_HARNESS=H))
sys.exit(r.returncode)

#!/bin/bash
# NTFS-1b stage 1: dirty guard + read-only guard tests. Clones fixtures under s1b/.
H=$(cd "$(dirname "$0")/.." && pwd)/build/ntfsharness
IMG=/Volumes/Media/spikes/ntfs/images
D=/Volumes/Media/spikes/ntfs/s1b
mkdir -p "$D"; pass=0; fail=0
ok() { echo "PASS $1"; pass=$((pass+1)); }
no() { echo "FAIL $1"; fail=$((fail+1)); }
sha() { shasum -a 256 "$1" | cut -c1-64; }
clone() { rm -f "$D/$2"; cp -c "$IMG/$1" "$D/$2"; }

# T1/T2: hibernated and dirty volumes mount read-only; a write is refused (EROFS) and the image is untouched
for f in hiber dirty; do
  clone $f.img t-$f.img; before=$(sha "$D/t-$f.img")
  out=$("$H" mkdir --image "$D/t-$f.img" --rw /guarded 2>&1); rc=$?
  after=$(sha "$D/t-$f.img")
  want=$([ $f = hiber ] && echo hibernated || echo dirty)
  if [ $rc -ne 0 ] && echo "$out" | grep -q "guard: volume is.*$want" && echo "$out" | grep -q "EROFS\|errno 30" && [ "$before" = "$after" ]
  then ok "T-$f: rw request on $want volume -> read-only, write refused, sha unchanged"
  else no "T-$f: rc=$rc out=[$out] sha_same=$([ "$before" = "$after" ] && echo 1 || echo 0)"; fi
  mt=$("$H" mounttest --image "$D/t-$f.img" 2>/dev/null | grep '^mode=rw ')
  echo "$mt" | grep -q "readonly=1 guard=" && ok "T-$f-mounttest: $mt" || no "T-$f-mounttest: $mt"
  # reads still work read-only
  "$H" ls --image "$D/t-$f.img" / >/dev/null 2>&1 && ok "T-$f-read: ls works" || no "T-$f-read"
done

# T3: --force-dirty-rw overrides the read-only guard on the dirty fixture
clone dirty.img t-force.img
"$H" mkdir --image "$D/t-force.img" --rw --force-dirty-rw /forced >/dev/null 2>&1 && "$H" stat --image "$D/t-force.img" /forced >/dev/null 2>&1 \
  && ok "T-force: --force-dirty-rw writes" || no "T-force"

# T4: clean volume: rw write works, clean unmount leaves dirty=0
clone probe1g.img t-clean.img
"$H" setflag --image "$D/t-clean.img" --rw clean >/dev/null 2>&1
"$H" mkdir --image "$D/t-clean.img" --rw /ok >/dev/null 2>&1 && ok "T-clean-write" || no "T-clean-write"
o=$("$H" flags --image "$D/t-clean.img"); rc=$?
[ $rc -eq 0 ] && echo "$o" | grep -q "dirty=0" && ok "T-clean-umount: $o" || no "T-clean-umount: rc=$rc $o"

# T5: kill -9 mid-run with the default guard leaves dirty=1, and the next rw mount is read-only
run_kill() { # $1 image, $2.. extra opts
  local img=$1; shift
  "$H" bench --image "$img" --rw "$@" --workload small --count 200000 --manifest "$D/k.manifest" >/dev/null 2>&1 &
  local pid=$!
  for i in $(seq 1 200); do [ -s "$D/k.manifest" ] && [ "$(grep -c '^D' "$D/k.manifest")" -ge 50 ] && break; sleep 0.05; done
  kill -9 $pid; wait $pid 2>/dev/null
}
clone probe1g.img t-kill.img; rm -f "$D/k.manifest"
run_kill "$D/t-kill.img"
o=$("$H" flags --image "$D/t-kill.img"); rc=$?
[ $rc -eq 10 ] && echo "$o" | grep -q "dirty=1" && ok "T-kill9-dirty: $o" || no "T-kill9-dirty: rc=$rc $o"
out=$("$H" mkdir --image "$D/t-kill.img" --rw /after 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "guard: volume is dirty" && ok "T-kill9-ro: next rw mount read-only" || no "T-kill9-ro: $out"

# T6: control: --no-dirty-guard + kill -9 leaves dirty=0 (why the guard must be default)
clone probe1g.img t-kill-ng.img; rm -f "$D/k.manifest"
run_kill "$D/t-kill-ng.img" --no-dirty-guard
o=$("$H" flags --image "$D/t-kill-ng.img"); rc=$?
echo "$o" | grep -q "dirty=0" && ok "T-noguard-control: kill -9 without guard leaves $o (crash invisible)" || no "T-noguard-control: $o"

echo "guard-test pass=$pass fail=$fail"
rm -f "$D"/t-*.img "$D/k.manifest"
[ $fail -eq 0 ]

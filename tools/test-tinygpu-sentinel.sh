#!/bin/sh
# tools/tinygpu-sentinel.sh against a stand-in tree and HOME: every pin found (install.sh and each tinygrad checkout) in
# every cache (caller's XDG_CACHE_HOME, the tree's, the user's default) is checked and created; existing files are kept.
set -u
S=$(cd "$(dirname "$0")" && pwd)/tinygpu-sentinel.sh; T=$(mktemp -d /tmp/tgsent.XXXXXX); fails=0; n=0
ok() { n=$((n+1)); if eval "$1"; then :; else fails=$((fails+1)); echo "FAIL: $2"; fi; }
A=c0d024f9ff0e1dc8fdf217f255da7101d91e8323; B=1111111111111111111111111111111111111111
mkdir -p "$T/root/tinygrad-x/tinygrad/runtime/support" "$T/home" "$T/xdg"
echo "TINYGPU_URL=https://github.com/tinygrad/tinygpu_releases/raw/$A/TinyGPU.zip" > "$T/root/install.sh"
printf '  def ensure_app(cls):\n    commit = "%s"\n' "$B" > "$T/root/tinygrad-x/tinygrad/runtime/support/system.py"
run() { HOME="$T/home" EGPU_ROOT="$T/root" XDG_CACHE_HOME="$T/xdg" sh "$S" "$@"; }
out=$(run check); rc=$?
ok '[ $rc = 1 ]' "check with nothing present should exit 1 (got $rc)"
ok '[ $(echo "$out" | grep -c MISSING:) = 6 ]' "expected 6 missing (3 caches x 2 pins): $out"
mkdir -p "$T/home/Library/Caches/tinygrad/downloads"; echo real > "$T/home/Library/Caches/tinygrad/downloads/TinyGPU_$A.zip"
out=$(run create); rc=$?
ok '[ $rc = 0 ]' "create should exit 0 (got $rc): $out"
ok '[ $(echo "$out" | grep -c created:) = 5 ]' "expected 5 created, 1 present: $out"
ok '[ "$(cat "$T/home/Library/Caches/tinygrad/downloads/TinyGPU_$A.zip")" = real ]' "an existing file was overwritten"
for d in "$T/xdg/tinygrad" "$T/root/.cache/tinygrad" "$T/home/Library/Caches/tinygrad"; do for k in $A $B; do
  ok '[ -f "$d/downloads/TinyGPU_$k.zip" ]' "missing after create: $d/downloads/TinyGPU_$k.zip"; done; done
out=$(run check); rc=$?
ok '[ $rc = 0 ]' "check after create should exit 0 (got $rc): $out"
# a space in every path (EGPU_ROOT, HOME and XDG_CACHE_HOME): each cache is one path, not two relative ones
W="$T/with space"; mkdir -p "$W/ro ot/tinygrad-x/tinygrad/runtime/support" "$W/ho me" "$W/x dg"
cp "$T/root/install.sh" "$W/ro ot/"; cp "$T/root/tinygrad-x/tinygrad/runtime/support/system.py" "$W/ro ot/tinygrad-x/tinygrad/runtime/support/"
out=$(cd "$T" && HOME="$W/ho me" EGPU_ROOT="$W/ro ot" XDG_CACHE_HOME="$W/x dg" sh "$S" create); rc=$?
ok '[ $rc = 0 ] && [ $(echo "$out" | grep -c created:) = 6 ]' "create with spaces: rc $rc: $out"
for d in "$W/x dg/tinygrad" "$W/ro ot/.cache/tinygrad" "$W/ho me/Library/Caches/tinygrad"; do
  ok '[ -f "$d/downloads/TinyGPU_$A.zip" ] && [ -f "$d/downloads/TinyGPU_$B.zip" ]' "missing with spaces: $d"; done
ok '[ ! -e "$T/dg" ] && [ ! -e "$T/me" ] && [ ! -e "$T/ot" ]' "a split path created a relative directory: $(ls "$T")"
rm -rf "$T"
echo "test-tinygpu-sentinel: $n checks run, $fails failed"; [ $fails = 0 ]

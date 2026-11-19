#!/bin/sh
# pick-bin.sh — the binary directory a runner uses when BIN is not set: the tree built for the connected card.
# setup.sh arch reads the card's PCI id from IOKit (read-only; an exported ARCH wins, and with no recognised card it is
# sm_120a). sm_120a lives in cuda-shim/build/bin, every other ARCH in cuda-shim/build/bin-<arch>; bin-multi carries both
# families. A Mac that built the default tree first (no card attached) and then ARCH=sm_86 has both build/bin and
# bin-sm_86, and build/bin is the wrong one for a 3060. Prints the directory; says on stderr when it is not build/bin;
# exits 1 when nothing built fits the card.
R=${EGPU_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; B=$R/cuda-shim/build
a=$(sh "$R/setup.sh" arch 2>/dev/null); a=${a:-sm_120a}
if [ "$a" = sm_120a ]; then set -- "$B/bin" "$B/bin-multi"; else set -- "$B/bin-$a" "$B/bin-multi"; fi
for d; do
  [ -d "$d" ] || continue
  [ "$d" = "$B/bin" ] || echo "BIN=$d (ARCH=$a; set BIN to choose)" >&2
  echo "$d"; exit 0
done
echo "no binaries for ARCH=$a: neither $1 nor $2 exists - build them (ARCH=$a sh install.sh build) or set BIN" >&2
exit 1

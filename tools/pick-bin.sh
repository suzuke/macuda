#!/bin/sh
# pick-bin.sh — the binary directory a runner uses when BIN is not set: cuda-shim/build/bin (the default Blackwell
# tree) when it exists, else the tree built for the connected card - setup.sh arch reads its PCI id from IOKit,
# read-only - as cuda-shim/build/bin-<arch>, else bin-multi. A Mac with only an RTX 30 or 40 build has no build/bin,
# and a runner that guessed build/bin would fail on a binary that was never built. Prints the directory; says on
# stderr when it is not build/bin; exits 1 when there is nothing to run.
R=${EGPU_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; B=$R/cuda-shim/build
[ -d "$B/bin" ] && { echo "$B/bin"; exit 0; }
a=$(sh "$R/setup.sh" arch 2>/dev/null)
for d in ${a:+"$B/bin-$a"} "$B/bin-multi"; do
  [ -d "$d" ] && { echo "BIN=$d (no cuda-shim/build/bin; ${a:+ARCH=$a from the card; }set BIN to choose)" >&2; echo "$d"; exit 0; }
done
echo "no binaries: neither $B/bin nor ${a:+$B/bin-$a or }$B/bin-multi exists - build first (sh install.sh, or ARCH=<arch> sh install.sh build)" >&2
exit 1

#!/bin/sh
# The file that stops tinygrad from killing a serving TinyGPU server (M5 of the 2026-11-18 DART panic analysis).
#
# tinygrad's APLRemotePCIDevice.ensure_app() (tinygrad/runtime/support/system.py) runs `pkill -f TinyGPU` and reinstalls
# the app unless BOTH /Applications/TinyGPU.app and <cache>/downloads/TinyGPU_<commit>.zip exist - and it is skipped
# entirely when APL_REMOTE_SOCK is set. macuda's own tools set APL_REMOTE_SOCK (tools/nvmini.py); a Python program that
# opens an APL device WITHOUT it - a tinygrad script run outside env.sh - would kill a server that may have bus mastering
# on, which is the H2/H10 shape that panicked macOS. install.sh installs the app without that zip, so nothing stopped it.
# tinygrad checks only is_file(), so an empty file is enough; it is version-specific (the name carries the TinyGPU commit
# tinygrad pins), so every pin found is covered.
#
# <cache> is tinygrad's: $XDG_CACHE_HOME/tinygrad, else ~/Library/Caches/tinygrad. env.sh points XDG_CACHE_HOME into the
# tree, so both the tree's cache and the user's default one are checked - a program run with or without env.sh.
#
#   tools/tinygpu-sentinel.sh check    one line per (cache, commit); exit 1 if any is missing
#   tools/tinygpu-sentinel.sh create   create the missing ones (empty files); never touches an existing file
R=${EGPU_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}

# Every TinyGPU commit something here pins: the app install.sh downloads, and each tinygrad checkout's ensure_app().
commits() {
  { sed -n 's#.*tinygpu_releases/raw/\([0-9a-f]\{40\}\)/TinyGPU.zip.*#\1#p' "$R/install.sh"
    for f in "$R"/tinygrad*/tinygrad/runtime/support/system.py; do
      [ -f "$f" ] && sed -n 's/^ *commit = "\([0-9a-f]\{40\}\)".*/\1/p' "$f"
    done; } 2>/dev/null | sort -u
}
# The caches tinygrad may use: the one in the caller's environment, the tree's (env.sh), and the user's default.
caches() {
  { [ -n "${XDG_CACHE_HOME:-}" ] && echo "$XDG_CACHE_HOME/tinygrad"
    echo "$R/.cache/tinygrad"
    echo "$HOME/Library/Caches/tinygrad"; } | awk '!seen[$0]++'
}

case "${1:-check}" in
  check|create)
    cs=$(commits); [ -n "$cs" ] || { echo "   no TinyGPU commit pin found (install.sh, tinygrad*/): nothing to check"; exit 0; }
    missing=0
    for c in $(caches); do for k in $cs; do
      f="$c/downloads/TinyGPU_$k.zip"
      if [ -f "$f" ]; then echo "   present: $f"
      elif [ "$1" = create ] && mkdir -p "$c/downloads" && : > "$f"; then echo "   created: $f"
      else echo "   MISSING: $f"; missing=1; fi
    done; done
    [ $missing = 0 ] || { echo "   a tinygrad APL program run without APL_REMOTE_SOCK would \`pkill -f TinyGPU\` the serving server; fix: $0 create"; exit 1; } ;;
  *) echo "usage: $0 check|create"; exit 2 ;;
esac

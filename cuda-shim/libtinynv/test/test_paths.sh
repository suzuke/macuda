#!/bin/sh
# A release unpacked anywhere finds its own helper, firmware and server script (src/exe_path.h), and a developer build
# finds the build tree's exactly as before and says nothing about it. The binaries are copied into temp <prefix>/bin and
# <prefix>/libexec/macuda directories; HOME is a temp directory throughout, so the real ~/Library/Logs is never written.
#
#   test_paths            (test/test_paths.c) the library's lookups, built with the build tree's defaults
#   test_paths_nobake     the same, built with defaults that do not exist: what a release sees on another machine
#   tinynv-bmclear-paths  the helper, its baked log in BUILD/paths-bakedlogs (made and removed here) and its baked
#                         script absent, asked with --paths
#
#   test_paths.sh BUILD_DIR BAKED_FIRMWARE_DIR
B=$1 FW=$2
[ -x "$B/test_paths" ] && [ -x "$B/test_paths_nobake" ] && [ -x "$B/tinynv-bmclear-paths" ] && [ -n "$FW" ] || { echo "usage: $0 BUILD_DIR BAKED_FIRMWARE_DIR"; exit 2; }
checks=0 fails=0
check() { checks=$((checks+1)); if ! eval "$1"; then fails=$((fails+1)); echo "FAIL test_paths: $2"; fi; }
T=$(mktemp -d "${TMPDIR:-/tmp}/tnpaths.XXXXXX") || exit 2
T=$(cd "$T" && pwd -P)   # what realpath gives the executable's directory: /private/var/..., not /var/...
export HOME=$T/home
mkdir -p "$HOME"
unset TINYNV_BMCLEAR_HELPER TINYNV_FW_DIR TINYGPU_SERVER_LOG TINYGPU_SERVER_SCRIPT
run() { "$@" > "$T/out" 2> "$T/err"; }   # stdout and stderr of one run, kept apart
val() { sed -n "s/^$1 //p" "$T/out"; }
nerr() { grep -c "$1" "$T/err" | tr -d ' '; }
stand_in() { mkdir -p "$(dirname "$1")"; printf '#!/bin/sh\nexit 0\n' > "$1"; chmod 755 "$1"; }

# ---- a developer build, in place: the build tree's helper and firmware, and not a line on stderr ----
run "$B/test_paths"
check '[ "$(val helper)" = "$B/tinynv-bmclear" ]' "developer build: helper $(val helper), want the build tree's $B/tinynv-bmclear"
check '[ "$(val firmware)" = "$FW" ]' "developer build: firmware $(val firmware), want the build tree's $FW"
check '[ ! -s "$T/err" ]' "developer build: stderr is not empty: $(cat "$T/err")"

# ---- a release: <prefix>/bin/X with the helper in <prefix>/libexec/macuda and the firmware in share/macuda ----
P=$T/p1
mkdir -p "$P/bin" "$P/share/macuda/firmware"
cp "$B/test_paths" "$P/bin/"
stand_in "$P/libexec/macuda/tinynv-bmclear"
run "$P/bin/test_paths"
check '[ "$(val helper)" = "$P/libexec/macuda/tinynv-bmclear" ]' "release: helper $(val helper), want $P/libexec/macuda/tinynv-bmclear"
check '[ "$(val firmware)" = "$P/share/macuda/firmware" ]' "release: firmware $(val firmware), want $P/share/macuda/firmware"
check '[ "$(val same)" = 1 ]' "release: a second lookup returned another path"
check '[ "$(nerr "libtinynv: tinynv-bmclear from $P/libexec/macuda/tinynv-bmclear (relative to the executable")" = 1 ]' \
      "release: the helper's path was not said exactly once: $(cat "$T/err")"
check '[ "$(nerr "libtinynv: firmware from $P/share/macuda/firmware (relative to the executable")" = 1 ]' \
      "release: the firmware's path was not said exactly once: $(cat "$T/err")"
check '[ "$(wc -l < "$T/err" | tr -d " ")" = 2 ]' "release: stderr has other lines: $(cat "$T/err")"
# beside the executable comes first
stand_in "$P/bin/tinynv-bmclear"
run "$P/bin/test_paths"
check '[ "$(val helper)" = "$P/bin/tinynv-bmclear" ]' "release: with a helper in bin/ too, helper $(val helper), want $P/bin/tinynv-bmclear"
# a helper that is not executable is not a helper
chmod 644 "$P/bin/tinynv-bmclear"
run "$P/bin/test_paths"
check '[ "$(val helper)" = "$P/libexec/macuda/tinynv-bmclear" ]' "release: a non-executable bin/tinynv-bmclear was taken ($(val helper))"
# the environment overrides win, and nothing is said about the lookup they replace
run env TINYNV_BMCLEAR_HELPER=/elsewhere/tinynv-bmclear TINYNV_FW_DIR=/elsewhere/firmware "$P/bin/test_paths"
check '[ "$(val helper)" = /elsewhere/tinynv-bmclear ] && [ "$(val firmware)" = /elsewhere/firmware ]' \
      "release: the overrides did not win: helper $(val helper), firmware $(val firmware)"
check '[ ! -s "$T/err" ]' "release: with both overrides set, stderr is not empty: $(cat "$T/err")"

# ---- a release with no helper anywhere: said loudly, once ----
P=$T/p2
mkdir -p "$P/bin"
cp "$B/test_paths_nobake" "$P/bin/"
run "$P/bin/test_paths_nobake"
check '[ "$(nerr "libtinynv: no tinynv-bmclear found (looked in $P/bin/tinynv-bmclear, $P/libexec/macuda/tinynv-bmclear, /nonexistent/baked/tinynv-bmclear): no emergency bus-master clear and no watchdog")" = 1 ]' \
      "no helper: the loud line is not there exactly once: $(cat "$T/err")"
check '[ "$(wc -l < "$T/err" | tr -d " ")" = 1 ]' "no helper: stderr has other lines: $(cat "$T/err")"
check '[ "$(val helper)" = /nonexistent/baked/tinynv-bmclear ] && [ "$(val firmware)" = /nonexistent/baked/firmware ]' \
      "no helper: the baked names are not what is left to fail with: helper $(val helper), firmware $(val firmware)"
run env TINYNV_BMCLEAR_HELPER=/elsewhere/tinynv-bmclear "$P/bin/test_paths_nobake"
check '[ ! -s "$T/err" ]' "no helper but an override: stderr is not empty: $(cat "$T/err")"

# ---- the helper's own lookups: the script beside it, the log in ~/Library/Logs/macuda ----
P=$T/p3
mkdir -p "$P/bin" "$P/libexec/macuda" "$P/share/macuda"
printf '#!/bin/sh\n' > "$P/share/macuda/tinygpu-server.sh"
cp "$B/tinynv-bmclear-paths" "$P/libexec/macuda/"
cp "$B/tinynv-bmclear-paths" "$P/bin/"
rm -rf "$B/paths-bakedlogs"
for h in "$P/libexec/macuda/tinynv-bmclear-paths" "$P/bin/tinynv-bmclear-paths"; do
  run "$h" --paths
  check '[ "$(val script)" = "$P/share/macuda/tinygpu-server.sh" ]' "helper at $h: script $(val script), want $P/share/macuda/tinygpu-server.sh"
  check '[ "$(val log)" = "$HOME/Library/Logs/macuda/tinygpu-server.log" ] && [ -d "$HOME/Library/Logs/macuda" ]' \
        "helper at $h: log $(val log), want $HOME/Library/Logs/macuda/tinygpu-server.log with its directory made"
done
# the baked log when its directory exists, as before
mkdir -p "$B/paths-bakedlogs"
run "$P/bin/tinynv-bmclear-paths" --paths
check '[ "$(val log)" = "$B/paths-bakedlogs/tinygpu-server.log" ]' "helper: with the baked log's directory there, log $(val log)"
rm -rf "$B/paths-bakedlogs"
# no script beside it: the baked one; no HOME: /dev/null
rm "$P/share/macuda/tinygpu-server.sh"
run env -u HOME "$P/bin/tinynv-bmclear-paths" --paths
check '[ "$(val script)" = /nonexistent/baked/tinygpu-server.sh ]' "helper: with no script beside it, script $(val script), want the baked one"
check '[ "$(val log)" = /dev/null ]' "helper: with no log directory and no HOME, log $(val log), want /dev/null"
# the overrides are terminal, even naming nothing (the tests' way to get the direct spawn)
run env TINYGPU_SERVER_SCRIPT=/nonexistent/tools/tinygpu-server.sh TINYGPU_SERVER_LOG=/elsewhere/server.log "$P/bin/tinynv-bmclear-paths" --paths
check '[ "$(val script)" = /nonexistent/tools/tinygpu-server.sh ] && [ "$(val log)" = /elsewhere/server.log ]' \
      "helper: the overrides did not win: script $(val script), log $(val log)"

rm -rf "$T"
echo "test_paths: $checks checks run, $fails failed"
[ $fails = 0 ]

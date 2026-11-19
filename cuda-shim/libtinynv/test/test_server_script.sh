#!/bin/sh
# tools/tinygpu-server.sh driven entirely by stand-ins: TINYGPU_SOCK in a fresh /tmp directory, TINYGPU_APP a fake server
# (test_watchdog's `server SOCK` role, which listens and answers), EGPU_ROOT a temp root. Nothing here may reach the
# real socket, the real server or the repository's logs/, and the end checks that none of them changed.
#
#   start, status, start again   listening; listening; refused (already listening)
#   kill -9 the server           the log has "server exit status 137", EGPU_ROOT/logs both DART snapshots; status fails
#   TINYGPU_SERVER_LOG           the server's output goes there instead, the snapshots still to EGPU_ROOT/logs
#   TINYGPU_START_NOWAIT=1       start returns at once without probing the socket; the server comes up all the same
#                                and is stopped by SIGTERM (143)
#
#   test_server_script.sh /path/to/tinygpu-server.sh /path/to/stand-in-app /path/to/repo/logs
SCRIPT=$1 STANDIN=$2 REPO_LOGS=$3
[ -f "$SCRIPT" ] && [ -x "$STANDIN" ] && [ -n "$REPO_LOGS" ] || { echo "usage: $0 SCRIPT STANDIN_APP REPO_LOGS_DIR"; exit 2; }
logs_of() { if [ -d "$1" ]; then ls "$1"; else echo "(no such directory)"; fi; }   # a worktree may have no logs/ yet
checks=0 fails=0
check() { checks=$((checks+1)); if ! eval "$1"; then fails=$((fails+1)); echo "FAIL test_server_script: $2"; fi; }

real_sock=${TMPDIR}tinygpu.sock
real_stat() { [ -e "$real_sock" ] && stat -f '%i %m' "$real_sock" || echo absent; }
real_before=$(real_stat)
server_before=$(pgrep -f "TinyGPU server" | tr '\n' ' ')
logs_before=$(logs_of "$REPO_LOGS")

d=$(mktemp -d /tmp/tgss.XXXXXX) || exit 2
export TINYGPU_SOCK=$d/tinygpu.sock TINYGPU_APP=$STANDIN EGPU_ROOT=$d/root
unset TINYGPU_SERVER_LOG TINYGPU_START_NOWAIT
[ "$TINYGPU_SOCK" != "$real_sock" ] || { echo "test_server_script: refusing, the socket is the real one"; exit 2; }
log=$d/root/logs/tinygpu-server.log
# the stand-in's pid: the process whose argv is exactly "APP server SOCK"
app_pid() { ps -axo pid=,args= | awk -v w="$STANDIN server $TINYGPU_SOCK" '{p=$1; $1=""; sub(/^ /, ""); if ($0 == w) print p}'; }
# until a file has a line, or a file exists, for up to 12 s
wait_grep() { i=0; while ! grep -q "$2" "$1" 2>/dev/null && [ $i -lt 120 ]; do i=$((i+1)); sleep 0.1; done; grep -q "$2" "$1" 2>/dev/null; }
wait_snaps() { i=0; while [ "$(ls "$d/root/logs" | grep -c "^dart-at-server-exit-.*\.txt$1\$")" -lt "$2" ] && [ $i -lt 120 ]; do i=$((i+1)); sleep 0.1; done; }
nsnaps() { ls "$d/root/logs" 2>/dev/null | grep -c "^dart-at-server-exit-.*\.txt$1\$"; }

out=$(sh "$SCRIPT" start 2>&1); rc=$?
check '[ $rc = 0 ] && echo "$out" | grep -q "listening on $TINYGPU_SOCK"' "start: rc $rc, output: $out"
out=$(sh "$SCRIPT" status 2>&1); rc=$?
check '[ $rc = 0 ] && echo "$out" | grep -q "listening on $TINYGPU_SOCK"' "status: rc $rc, output: $out"
out=$(sh "$SCRIPT" start 2>&1); rc=$?
check '[ $rc = 1 ] && echo "$out" | grep -q "already listening"' "a second start: rc $rc, output: $out"
p=$(app_pid)
check '[ -n "$p" ] && [ "$(echo $p | wc -w)" -eq 1 ]' "want one stand-in server process, found '$p'"
[ -n "$p" ] && kill -9 $p
check 'wait_grep "$log" "server exit status 137"' "kill -9: no 'server exit status 137' in $log"
wait_snaps .5s 1
check '[ "$(nsnaps "")" = 1 ] && [ "$(nsnaps .5s)" = 1 ]' "kill -9: want both DART snapshots in $d/root/logs, have: $(ls "$d/root/logs" | tr '\n' ' ')"
out=$(sh "$SCRIPT" status 2>&1); rc=$?
check '[ $rc = 1 ] && echo "$out" | grep -q "NOT listening"' "status after the kill: rc $rc, output: $out"

# the log elsewhere, and a start that does not wait
export TINYGPU_SERVER_LOG=$d/other.log TINYGPU_START_NOWAIT=1
out=$(sh "$SCRIPT" start 2>&1); rc=$?
check '[ $rc = 0 ] && echo "$out" | grep -q "not waiting for the socket"' "nowait start: rc $rc, output: $out"
i=0; while [ ! -S "$TINYGPU_SOCK" ] && [ $i -lt 100 ]; do i=$((i+1)); sleep 0.1; done
out=$(sh "$SCRIPT" status 2>&1); rc=$?
check '[ $rc = 0 ]' "nowait: the server did not come up: $out"
p=$(app_pid)
[ -n "$p" ] && kill -TERM $p
check 'wait_grep "$TINYGPU_SERVER_LOG" "server exit status 143"' "SIGTERM: no 'server exit status 143' in $TINYGPU_SERVER_LOG"
wait_snaps .5s 2
check '[ "$(nsnaps .5s)" = 2 ] && ! grep -q "exit status 143" "$log"' "TINYGPU_SERVER_LOG: the snapshots are not in EGPU_ROOT/logs, or the output went to the default log"

# nothing real touched
check '[ "$(real_stat)" = "$real_before" ]' "the real socket $real_sock changed: $real_before -> $(real_stat)"
check '[ "$(pgrep -f "TinyGPU server" | tr "\n" " ")" = "$server_before" ]' "the real server changed: '$server_before'"
check '[ "$(logs_of "$REPO_LOGS")" = "$logs_before" ]' "the repository's logs/ changed"
[ -z "$(app_pid)" ] || kill -9 $(app_pid)
rm -rf "$d"
echo "test_server_script: $checks checks run, $fails failed"
[ $fails = 0 ]

#!/bin/sh
# The TinyGPU server: is it there, and start it if not.
#
# A replug takes it with it. That has happened three times now and cost a confused minute each time, because the socket
# file survives and a client gets "connection refused" rather than "nothing is listening" - which reads like a driver
# failure rather than a missing daemon. So: ask, and start it the way it was running.
#
# But "connection refused" is NOT proof the server is gone: on macOS a listening unix socket refuses connects while its
# backlog is full, and on 2026-09-15 06:53 a live server refused one client for a moment, `ensure` took that for death,
# unlinked its socket and started a second server on the same card. Now: a live server PROCESS for this socket is never
# replaced. A refusal with the process alive is retried for two seconds and then reported as busy, exit 1, so a runner
# aborts its step rather than run against a replacement.
#
#
# HOW IT DIED IS RECORDED (2026-11-18 DART panic, M6). That night the server's session ended mid-upload and nobody can say
# why: its stdout was block-buffered into the log (16384 bytes, cut mid-line, last written hours earlier), no wrapper saw
# it exit, and the reboot erased the latched pci-dart-error-data. So the server now runs under script(1), whose pty makes
# its output line-buffered; the shell inside the pty writes the server's exit status (128+N = killed by signal N); and the
# subshell that started it - whose command line does not contain "TinyGPU", so `pkill -f TinyGPU` cannot take it too -
# writes script's own status and snapshots the DART property, at once and five seconds later. No inner status line
# above the outer one means the shell around the server was killed with it, which is what a `pkill -f` looks like.
#
#   tools/tinygpu-server.sh status | start | ensure
SOCK=${TMPDIR}tinygpu.sock
APP=${TINYGPU_APP:-/Applications/TinyGPU.app/Contents/MacOS/TinyGPU}   # TINYGPU_APP: a stand-in, for testing this script with no card
R=${EGPU_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}; LOG=$R/logs/tinygpu-server.log; mkdir -p "$R/logs"

alive() { python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try: s.connect('$SOCK')
except OSError: sys.exit(1)
finally: s.close()
"; }
# the latched DART fault record, searched the way tools/dart-stale.sh and preflight search it
dart_snap() { { echo "when=$(date -Iseconds)"; ioreg -l -w0 2>/dev/null | grep -o '"pci-dart-error-data" = <[0-9a-f]*>' || echo "pci-dart-error-data: absent"; } > "$1"; }
serving() { pgrep -f "TinyGPU server $SOCK" | tr '\n' ' '; }   # every server process bound to this socket path (should be one)

case "${1:-status}" in
  status) p=$(serving); if alive; then echo "tinygpu server: listening on $SOCK (pid ${p:-?})"; else
            echo "tinygpu server: NOT listening on $SOCK$([ -S "$SOCK" ] && echo ' (the socket file is stale)')${p:+; server process(es) $p exist but refuse connections}"; exit 1; fi ;;
  start|ensure)
    if alive; then [ "$1" = ensure ] && { echo "tinygpu server: already listening (pid $(serving))"; exit 0; }
                   echo "tinygpu server: already listening; stop it first to restart"; exit 1; fi
    p=$(serving)
    if [ -n "$p" ]; then
      i=0; while [ $i -lt 20 ]; do i=$((i+1)); sleep 0.1; alive && { echo "tinygpu server: listening on $SOCK (pid $p) after a refused connect — it was busy"; exit 0; }; done
      echo "tinygpu server: process(es) $p exist for $SOCK but refuse connections after 2 s — busy or wedged, NOT starting another"; exit 1
    fi
    rm -f "$SOCK"
    (
      trap '' HUP   # what nohup did; ignored dispositions survive exec, so script and the server inherit it
      script -q /dev/null /bin/sh -c '"$0" server "$1"; rc=$?; echo "$(date "+%F %T") tinygpu-server.sh: server exit status $rc (128+N = killed by signal N)"; exit $rc' \
        "$APP" "$SOCK" < /dev/null >> "$LOG" 2>&1
      rc=$?; ts=$(date +%Y%m%d-%H%M%S)
      echo "$(date '+%F %T') tinygpu-server.sh: script(1) around the server exited $rc (the inner shell's exit code, or the signal that killed it - no status line just above means the shell went with the server); DART snapshots: $R/logs/dart-at-server-exit-$ts.txt{,.5s}" >> "$LOG"
      dart_snap "$R/logs/dart-at-server-exit-$ts.txt"; sync
      sleep 5; dart_snap "$R/logs/dart-at-server-exit-$ts.txt.5s"; sync
    ) > /dev/null 2>&1 &
    echo "started (wrapper pid $!), waiting for the socket"
    i=0; while [ ! -S "$SOCK" ] && [ $i -lt 100 ]; do i=$((i+1)); sleep 0.1; done
    if alive; then echo "tinygpu server: listening on $SOCK"; else echo "tinygpu server: did not come up; see $LOG"; exit 1; fi ;;
  *) echo "usage: $0 status|start|ensure"; exit 2 ;;
esac

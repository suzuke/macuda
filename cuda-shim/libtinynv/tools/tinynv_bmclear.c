// tinynv-bmclear: turn the card's bus mastering off through a FRESH connection to the TinyGPU server, for the moment a
// client has lost its own (M3 of the 2026-11-18 DART panic analysis). That night the server's session ended, the dext
// completed every DMA mapping with bus mastering still on, and a running GSP-RM faulted on every write for 32 s until
// the kernel panicked. A card that is not a bus master cannot DMA at all, so clearing that one bit ends the storm.
//
// libtinynv starts this once, when it latches a lost server (pci_tinygpu.c: tg_rescue). It is a separate process and
// stops itself after --timeout seconds, because opening the device again can block behind the dead session in the
// dext's serial queue (H3), and nothing that blocks may take the client down with it.
//
//   tinynv-bmclear --sock PATH [--old-pid N] [--app PATH] [--timeout S] [--reply-timeout S] [--no-start]
//
// 1. A server for PATH: if nothing accepts a connection, and the old server (--old-pid) is gone, and no process at all
//    is serving PATH (argv "... server PATH", as tools/tinygpu-server.sh checks with pgrep), one is started the way
//    that script starts it - BY that script, `sh tools/tinygpu-server.sh start`, which runs `APP server PATH` under
//    script(1) and records how it ends (M6; start_server says how, and when it falls back to starting the app itself)
//    - in a session of its own so it outlives this. --app, else $TINYGPU_APP, else the installed app. A live server
//    that refuses connections is busy or wedged and is never replaced.
// 2. The first request is a READ of PCI_COMMAND, and nothing is written until it is answered. The server takes one
//    client at a time (listen(1), one thread), so a connect alone proves nothing - the backlog accepts it while
//    another client is being served. An answer within --reply-timeout proves this connection is the one served.
// 3. All ones (0xffff, or the transport's 0xffffffff) is a device that is not there: nothing is written.
// 4. PCI_COMMAND & ~BUS_MASTER written, read back, and the result said.
// No FLR: under a live fault storm it is untested, and bus mastering off is the half that cannot make things worse.
//
// Every line goes to stderr - which libtinynv shares, so the result lands in the client's own log. Exit status:
// 0 bus mastering confirmed off; 2 usage; 3 no server reachable and none started; 4 the first read was not answered
// (another client, or a wedged server) - nothing written; 5 the server failed a request or the connection was lost;
// 6 the device reads all ones - nothing written; 7 the read-back still has bus mastering on; 124 --timeout expired.
//
// ONE SERVER START, however many clears race. The client's own rescue and the watchdog's server-death trigger fired
// in the same millisecond on the card (2026-11-19), and two helpers that each find no server each start one: two
// TinyGPU servers on one socket path. So steps 1-4 run under flock on "PATH.bmclear.lock", in every mode, and a helper
// that had to wait for the lock never starts a server: whoever held it has started one or found one, and the waiter
// only looks again (a connect, then the process list). The lock file is never removed - an unlink between another
// helper's open and its flock would hand the two of them different locks.
//
//   tinynv-bmclear --watch [--old-pid N]       (socket in $TINYNV_BMCLEAR_SOCK, the client's clean channel on fd 3)
//   tinynv-bmclear --paths                     the server script and log a start would use (server_script, server_log)
//
// WATCH MODE: M4, the watchdog libtinynv starts beside every TinyGPU session (pci_tinygpu.c: tg_watch). M2 and M3 need
// a client alive to notice; two ways out had nobody: the client killed or crashed under a live server, which then
// tears the session down with bus mastering on, and the server dying under an idle client that touches nothing. This
// process outlives both and runs steps 1-4:
//   - the client gone (the last write end of fd 3 closed: no process holds the session) while ARMED: "client exited
//     without a clean teardown";
//   - the server (--old-pid, the client's LOCAL_PEERPID) exited while the client lives and is armed: "server pid N
//     exited", a fresh server started if none serves the path. A clear that confirmed bus mastering off is the
//     last: it waits on for the client and clears no more. One that failed is tried again while the client lives
//     and stays armed (after 5 s, then 10, 20, 40, 80), and once more when the client goes.
// The client writes one byte each time: 'A' bus mastering may be on (EVERY PCI_COMMAND write with MASTER, EVERY DMA
// mapping), 'C' its own teardown read bus mastering back OFF. The LAST byte received decides, whichever process of
// the session sent it - a fork shares the pipe, so the client keeps no "already said" of its own. A quiesce in
// mid-session followed by a reboot is armed again; a session that never turned mastering on is clean, and a clean
// exit makes NO connection to the server - one spare connect sits in the listen(1) backlog and the next program's
// connect is refused. Any other byte is ignored.
// NOTHING STANDS THIS DOWN BUT A CLEAN STATE. The client's own M3 rescue once did, with a byte sent when it started;
// the rescue then timed out behind a server 3 s in the dead session's teardown, and nobody was left (found by
// review, 2026-11-19). Now the two just run in a row under the lock above: the second connects to the server that is
// up, reads PCI_COMMAND, writes only if MASTER is still set, exits 0 - and, having waited, starts no server.
// The socket is not in argv because tools that stop a server run `pkill -f tinygpu.sock`, which would take this too.
// A live server that refuses the connect (its listen(1) backlog full), or that has no socket file yet (still
// starting), is tried again until the hard timeout: the watchdog is the last one there, and exit 3 would leave nobody.
// SIGINT, SIGHUP and SIGTERM are ignored and the process has a session of its own: a ^C or a group kill of the client
// is exactly what it is there for. Its descriptors are fd 3 and stdio only - holding the client's server socket would
// keep the session open after the client died, and the server would never see it go; stdout and stderr are the
// client's stderr, or /dev/null when the client has none of its own (pci_tinygpu.c: tg_child_stdio). The client killed under a busy
// server meets a server still tearing the dead session down, so the first read may wait: --reply-timeout 8 and
// --timeout 10 here, against 1 and 5 for the client's own rescue, where nothing else is being torn down.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <sys/event.h>
#include <sys/sysctl.h>
#endif
#include <libgen.h>
#include <limits.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "../src/exe_path.h"

extern char **environ;

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_CFG_READ = 3, CMD_CFG_WRITE = 4 };
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x4
#define DEFAULT_APP "/Applications/TinyGPU.app/Contents/MacOS/TinyGPU"
// tools/tinygpu-server.sh's log, which the Makefile bakes in from the repo root; TINYGPU_SERVER_LOG overrides it
#ifndef TINYNV_SERVER_LOG_DEFAULT
#define TINYNV_SERVER_LOG_DEFAULT ""
#endif
static const char *server_log_default = TINYNV_SERVER_LOG_DEFAULT;
// and tools/tinygpu-server.sh itself, which starts the server the way M6 records it; TINYGPU_SERVER_SCRIPT overrides it
#ifndef TINYNV_SERVER_SCRIPT_DEFAULT
#define TINYNV_SERVER_SCRIPT_DEFAULT ""
#endif
static const char *server_script_default = TINYNV_SERVER_SCRIPT_DEFAULT;

// Where those two are for THIS helper, which may be a release unpacked anywhere rather than the build tree the defaults
// above name (src/exe_path.h has the layout; the prefix is the parent of bin/, or of libexec/macuda/ when the helper sits
// there). An environment override is terminal even when it names nothing: the tests point TINYGPU_SERVER_SCRIPT at a
// path that does not exist to get the direct spawn, and falling through would start their servers through the real
// script instead.
//   script: TINYGPU_SERVER_SCRIPT, else <prefix>/share/macuda/tinygpu-server.sh if readable, else the baked one.
//   log:    TINYGPU_SERVER_LOG, else the baked repo log when its directory exists, else
//           $HOME/Library/Logs/macuda/tinygpu-server.log (the directory created), else /dev/null.
static void server_script(char *out, size_t n) {
  const char *e = getenv("TINYGPU_SERVER_SCRIPT");
  char dir[PATH_MAX], pre[PATH_MAX], cand[PATH_MAX + 48];
  if (e && *e) { snprintf(out, n, "%s", e); return; }
  if (!tinynv_exe_dir(dir, sizeof(dir)) && !tinynv_exe_prefix(dir, pre, sizeof(pre))) {
    if (!tinynv_join(cand, sizeof(cand), pre, "/share/macuda/tinygpu-server.sh") && tinynv_is_readable(cand)
        && !tinynv_join(out, n, cand, "")) return;
  }
  snprintf(out, n, "%s", server_script_default);
}
static int mkdirs(char *p) {   // mkdir -p, in place; 0 when p is a directory afterwards
  for (char *s = p + 1; *s; s++)
    if (*s == '/') { *s = 0; int r = mkdir(p, 0755); *s = '/'; if (r && errno != EEXIST) return -1; }
  return mkdir(p, 0755) && errno != EEXIST ? -1 : !tinynv_is_dir(p);
}
static void server_log(char *out, size_t n) {
  const char *e = getenv("TINYGPU_SERVER_LOG"), *home = getenv("HOME");
  if (e && *e) { snprintf(out, n, "%s", e); return; }
  char d[PATH_MAX];
  snprintf(d, sizeof(d), "%s", server_log_default);
  char *slash = strrchr(d, '/');
  if (slash && slash != d) { *slash = 0; if (tinynv_is_dir(d)) { snprintf(out, n, "%s", server_log_default); return; } }
  if (home && *home && !tinynv_join(d, sizeof(d), home, "/Library/Logs/macuda") && !mkdirs(d)
      && !tinynv_join(out, n, d, "/tinygpu-server.log"))
    return;
  snprintf(out, n, "/dev/null");
}

static void say(const char *fmt, ...) {
  char ts[32];
  time_t now = time(NULL);
  strftime(ts, sizeof(ts), "%F %T", localtime(&now));
  fprintf(stderr, "tinynv-bmclear[%d] %s: ", (int)getpid(), ts);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
}

static void on_alarm(int sig) {
  (void)sig;
  static const char m[] = "tinynv-bmclear: the hard timeout expired; stopping with nothing more sent\n";
  ssize_t w = write(2, m, sizeof(m) - 1);   // nothing to do if it fails: the exit status says it
  (void)w;
  _exit(124);
}

static int pid_alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

// One spelling per socket: the directory resolved (symlinks, "//", /var vs /private/var), the name kept - the socket
// itself may be gone. So a server started as "server $TMPDIR/tinygpu.sock" is found whichever way the client spelt it.
static void norm(const char *path, char *out, size_t n) {
  char d[PATH_MAX], b[PATH_MAX], r[PATH_MAX];
  snprintf(d, sizeof(d), "%s", path);
  snprintf(b, sizeof(b), "%s", path);
  char dir_slash[PATH_MAX + 1];
  if (!realpath(dirname(d), r) || tinynv_join(dir_slash, sizeof(dir_slash), r, "/") || tinynv_join(out, n, dir_slash, basename(b)))
    snprintf(out, n, "%s", path);   // unresolvable, or too long resolved: the spelling as given
}

// Any process whose arguments contain "server PATH": a TinyGPU server for this socket, alive whatever its socket says.
// -1 where processes cannot be listed this way, which the caller treats as "there may be one".
static int server_process_for(const char *sock) {
#ifndef __APPLE__
  (void)sock;
  return -1;
#else
  char want[PATH_MAX], got[PATH_MAX];
  norm(sock, want, sizeof(want));
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0}, amib[2] = {CTL_KERN, KERN_ARGMAX}, argmax = 0, found = 0;
  size_t len = 0, sz = sizeof(argmax);
  if (sysctl(mib, 3, NULL, &len, NULL, 0) || sysctl(amib, 2, &argmax, &sz, NULL, 0) || argmax <= 0) return 0;
  len += len / 8;
  struct kinfo_proc *kp = malloc(len);
  char *buf = malloc((size_t)argmax);
  if (kp && buf && !sysctl(mib, 3, kp, &len, NULL, 0)) {
    for (size_t i = 0; i < len / sizeof(*kp) && !found; i++) {
      int pid = kp[i].kp_proc.p_pid, pmib[3] = {CTL_KERN, KERN_PROCARGS2, pid}, argc;
      size_t bl = (size_t)argmax;
      if (pid == getpid() || sysctl(pmib, 3, buf, &bl, NULL, 0) || bl < sizeof(argc)) continue;
      memcpy(&argc, buf, sizeof(argc));
      char *q = buf + sizeof(argc), *end = buf + bl;
      while (q < end && *q) q++;   // the executable path, then its padding
      while (q < end && !*q) q++;
      const char *prev = NULL;
      for (int a = 0; a < argc && q < end; a++) {
        if (prev && !strcmp(prev, "server") && (norm(q, got, sizeof(got)), !strcmp(got, want))) { found = pid; break; }
        prev = q;
        q += strnlen(q, (size_t)(end - q)) + 1;
      }
    }
  }
  free(kp);
  free(buf);
  return found;
#endif
}

static int connect_to(const char *sock, double reply_timeout) {
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { int e = errno; close(fd); errno = e; return -1; }
  struct timeval tv = {(time_t)reply_timeout, (int)((reply_timeout - (double)(time_t)reply_timeout) * 1e6)};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

// Why this process is clearing, for the line the server log gets when a server is started here.
static const char *g_why = "the client lost its server connection (M3)";
// The watchdog's clears wait for a live server that refuses connections; a one-shot helper says so and exits 3.
static int g_patient;

// A server started here is a server like any other, as tools/tinygpu-server.sh starts it, not a child of the trouble:
//   - SIGINT, SIGTERM and SIGPIPE at their defaults, an empty mask. The watchdog ignores INT and TERM, and ignored
//     signals survive both its fork and the spawn, so a server it started could not be stopped by `pkill -f
//     tinygpu.sock` (found by review, 2026-11-19). SIGHUP stays ignored, as the script's `trap '' HUP` has it: the
//     server outlives the terminal it was started from.
//   - Its output goes to the server log the script writes (TINYGPU_SERVER_LOG, else the repo's
//     logs/tinygpu-server.log, baked in at build, else ~/Library/Logs/macuda: server_log), appended, with one line first saying who started it and why. Never
//     the client's stderr: a server holding that would keep a pipe reading the client's log open for its whole life.
//     A log that cannot be opened is /dev/null.
// THROUGH THE SCRIPT (M6 for this helper). A server started here exists only right after an incident, and is the one
// whose death matters most - yet started directly it left no exit status and no DART snapshot. So it is started as
// `/bin/sh SCRIPT start` (TINYGPU_SERVER_SCRIPT, else a release's share/macuda copy, else tools/tinygpu-server.sh baked in at
// build: server_script), which runs the app under script(1), writes its exit status and snapshots pci-dart-error-data
// from a subshell of its own that outlives this process. The script is handed the socket (TINYGPU_SOCK), the app (TINYGPU_APP), the log opened here
// (TINYGPU_SERVER_LOG, so the line below and the server's output are one file; /dev/null if it could not be opened,
// where the script's own `>> LOG` would otherwise fail and start nothing) and TINYGPU_START_NOWAIT=1: this process
// waits for the socket with its own connects, and the script's probe would be one more connect into the new server's
// listen(1) backlog. Everything above holds for the script as for the app, since it is the same spawn: the signals,
// CLOEXEC_DEFAULT (neither the lock nor any socket or pipe of the client's reaches the script or the server), stdio,
// and a session of its own. A script that is not there or not readable, or a /bin/sh that cannot be spawned, is the
// direct spawn as before, and the log says it fell back.
// The window this opens: until the script has exec'd the app, no process has "server PATH" in its argv, so
// server_process_for() cannot see a server coming up (the script itself and its subshell must not carry the socket in
// argv: `pkill -f tinygpu.sock` would take the subshell that keeps the records). That is harmless while the starter
// holds the lock and polls its connect, which lasts until it connects or its hard timeout; a lock waiter never starts
// one anyway. Only a starter whose timeout (5 s, 10 s watching) expires inside the window - which takes well under a
// second - would hand the lock to a later helper that sees nothing and starts a second; the script's own pgrep then
// refuses a second installed app that has exec'd by then.
static int start_server(const char *app, const char *sock) {
  struct stat st;
  if (!lstat(sock, &st) && !S_ISSOCK(st.st_mode)) { say("%s exists and is not a socket; not starting a server on it", sock); return -1; }
  unlink(sock);   // a stale socket file: nothing accepts on it (checked above) and no process serves it
  char why[PATH_MAX + 128] = "";   // why the output goes to /dev/null, when the log could not be opened
  char logp[PATH_MAX], scriptp[PATH_MAX];
  server_log(logp, sizeof(logp));
  server_script(scriptp, sizeof(scriptp));
  const char *log = logp;
  int lfd = open(log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (lfd < 0) {
    snprintf(why, sizeof(why), " (the server log %s could not be opened: %s)", log, strerror(errno));
    say("cannot open the server log %s (%s): the server's output goes to /dev/null", log, strerror(errno));
    log = "/dev/null";
    lfd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (lfd < 0) { say("cannot open /dev/null: %s; not starting a server", strerror(errno)); return -1; }
  }
  // an app that cannot run is a start that fails here, as the direct spawn's did, not a script waiting on nothing
  if (access(app, X_OK)) { int e = errno; close(lfd); say("could not start %s server %s: %s", app, sock, strerror(e)); return -1; }
  const char *script = scriptp;
  char fell[PATH_MAX + 160] = "";   // why the app is started directly, when it is
  if (!script || !*script) snprintf(fell, sizeof(fell), "no server script is configured (TINYGPU_SERVER_SCRIPT)");
  else if (access(script, R_OK)) snprintf(fell, sizeof(fell), "the server script %s cannot be read (%s)", script, strerror(errno));
  posix_spawnattr_t at;
  posix_spawn_file_actions_t fa;
  sigset_t def, none;
  sigemptyset(&def);
  sigaddset(&def, SIGINT);
  sigaddset(&def, SIGTERM);
  sigaddset(&def, SIGPIPE);
  sigemptyset(&none);
  signal(SIGHUP, SIG_IGN);   // in this process (a helper, or the watchdog's clear child), so the server inherits it
  posix_spawnattr_init(&at);
  posix_spawnattr_setsigdefault(&at, &def);
  posix_spawnattr_setsigmask(&at, &none);
  short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_SETSID
  flags |= POSIX_SPAWN_SETSID;
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&at, flags);
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, lfd, 1);
  posix_spawn_file_actions_adddup2(&fa, lfd, 2);
  pid_t pid;
  int rc = -1;
  if (!*fell) {
    // this process is a one-shot helper or the watchdog's clear child, both gone soon: the environment is theirs to set
    setenv("TINYGPU_SOCK", sock, 1);
    setenv("TINYGPU_APP", app, 1);
    setenv("TINYGPU_SERVER_LOG", log, 1);
    setenv("TINYGPU_START_NOWAIT", "1", 1);
    char *sargv[] = {"/bin/sh", (char *)script, "start", NULL};
    rc = posix_spawn(&pid, "/bin/sh", &fa, &at, sargv, environ);
    if (rc) snprintf(fell, sizeof(fell), "/bin/sh %s start could not be spawned (%s)", script, strerror(rc));
  }
  if (*fell) {
    say("%s: fell back to starting %s directly, with no exit status or DART snapshot recorded", fell, app);
    char *argv[] = {(char *)app, "server", (char *)sock, NULL};
    rc = posix_spawn(&pid, app, &fa, &at, argv, environ);
  }
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc) { close(lfd); say("could not start %s server %s: %s", app, sock, strerror(rc)); return -1; }
  char line[PATH_MAX * 4 + 256], ts[32];
  time_t now = time(NULL);
  strftime(ts, sizeof(ts), "%F %T", localtime(&now));
  int n = *fell ? snprintf(line, sizeof(line), "%s tinynv-bmclear[%d]: %s: fell back; started %s server %s directly (pid %d): %s\n", ts,
                           (int)getpid(), fell, app, sock, (int)pid, g_why)
                : snprintf(line, sizeof(line), "%s tinynv-bmclear[%d]: started %s server %s through %s start (the script's pid %d; "
                           "the lines below are its own): %s\n", ts, (int)getpid(), app, sock, script, (int)pid, g_why);
  if (n > 0 && write(lfd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1) < 0) {}
  close(lfd);
  if (*fell) say("started %s server %s (pid %d); its output goes to %s%s", app, sock, (int)pid, log, why);
  else say("started %s server %s through %s start (pid %d); its output goes to %s%s", app, sock, script, (int)pid, log, why);
  return 0;
}

// 0, -1 timed out (no answer), -2 connection lost or refused by the server
static int rpc(int fd, uint8_t cmd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *out) {
  req_t rq = {.cmd = cmd, .arg0 = a0, .arg1 = a1, .arg2 = a2};
  resp_t rp;
  if (send(fd, &rq, sizeof(rq), 0) != (ssize_t)sizeof(rq)) return errno == EAGAIN ? -1 : -2;
  for (size_t got = 0; got < sizeof(rp);) {
    ssize_t n = recv(fd, (char *)&rp + got, sizeof(rp) - got, 0);
    if (n > 0) { got += (size_t)n; continue; }
    if (n < 0 && errno == EINTR) continue;
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
  }
  if (rp.status) {
    char msg[256] = "";
    size_t n = rp.resp0 < sizeof(msg) - 1 ? (size_t)rp.resp0 : sizeof(msg) - 1;
    if (n && recv(fd, msg, n, MSG_WAITALL) > 0) msg[n] = 0;
    say("the server refused command %u: %s", cmd, msg);
    return -2;
  }
  if (out) *out = rp.resp0;
  return 0;
}

// Steps 1-4 under the lock. old_gone: the caller knows the old server has exited (the watchdog was told by the
// kernel), which a zombie not yet reaped would otherwise contradict.
static int clear(const char *sock, int old_pid, int old_gone, const char *app, double reply_timeout, int no_start) {
  int old_alive = !old_gone && pid_alive(old_pid);
  say("clearing bus mastering through %s (old server pid %d %s)", sock, old_pid,
      old_pid <= 0 ? "unknown" : old_alive ? "alive" : "gone");
  char lock[PATH_MAX];
  int waited = 0;
  char spelt[PATH_MAX];
  norm(sock, spelt, sizeof(spelt));   // one name however the socket is spelt, as server_process_for matches it
  // a cut name would be some other lock file: then none (a socket path is at most 104 bytes, so it never is)
  int lfd = tinynv_join(lock, sizeof(lock), spelt, ".bmclear.lock") ? (errno = ENAMETOOLONG, -1) : open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lfd < 0) say("no lock %s (%s): going on without one", lock, strerror(errno));
  else if (flock(lfd, LOCK_EX | LOCK_NB)) {
    say("another bus-master clear of %s holds %s: waiting for it", sock, lock);
    while (flock(lfd, LOCK_EX) && errno == EINTR) {}   // bounded by the alarm
    waited = 1;
    say("the other clear is done: looking again for its server, and starting none");
  }

  int fd = connect_to(sock, reply_timeout), ce = errno;   // ce: why the last connect failed, kept past other calls
  // A waiter that finds a server process not yet listening: most likely the one the lock holder started, which ran out
  // of time before it listened. Connects only, until it does or the alarm - nothing is replaced or started.
  for (struct timespec nap = {0, 50 * 1000 * 1000}; fd < 0 && waited && server_process_for(sock); nanosleep(&nap, NULL))
    fd = connect_to(sock, reply_timeout), ce = errno;
  // THE WATCHDOG WAITS FOR A FULL BACKLOG. It is the last one there, and a refused connect to a live server is most
  // often the listen(1) backlog holding somebody else's connection: the client's own rescue connected, timed out
  // behind the dead session's teardown and left, and its dead connection sits there until the server gets to it -
  // the watchdog, coming next, was refused and gave up with the card at 0x407 (2026-11-19). Connects only, while a
  // server for the path lives, until one is taken or the alarm; nothing is replaced, and if the server dies
  // meanwhile the lines below decide as they always did.
  // ENOENT THE SAME, while a server process for the path exists: that is a server somebody started and that has not
  // bound its socket yet (start_server unlinks the file first). The clear that started it timed out, the client was
  // killed, and this last clear saw no file, did not wait, and exited 3 with the card on (round 3 of the review).
  // The loop ends without a connection only when no server process is left; then, and only for a helper that did
  // not wait for the lock, the lines below may start one - never a second beside one that lives.
  if (fd < 0 && g_patient && (ce == ECONNREFUSED || ce == ENOENT) && (old_alive || server_process_for(sock))) {
    say("a server for %s is alive but %s: trying until it takes a connection", sock,
        ce == ENOENT ? "has no socket yet (still starting)" : "refuses connections (its backlog is full, or it is wedged)");
    for (struct timespec nap = {0, 100 * 1000 * 1000}; fd < 0; nanosleep(&nap, NULL)) {
      fd = connect_to(sock, reply_timeout), ce = errno;
      if (fd < 0 && !(old_alive = !old_gone && pid_alive(old_pid)) && !server_process_for(sock)) break;
    }
  }
  if (fd < 0) {
    int e = ce, other = server_process_for(sock);
    if (old_alive || other) {
      say("a server for %s may be alive (pid %d) but refuses connections (%s): busy with another client or wedged; not touching it",
          sock, old_alive ? old_pid : other, strerror(e));
      return 3;
    }
    if (waited) { say("no server on %s (%s) after the other clear: not starting a second; nothing done", sock, strerror(e)); return 3; }
    if (no_start) { say("no server on %s (%s) and --no-start: nothing done", sock, strerror(e)); return 3; }
    if (start_server(app, sock)) return 3;
    struct timespec nap = {0, 50 * 1000 * 1000};
    while ((fd = connect_to(sock, reply_timeout)) < 0) nanosleep(&nap, NULL);   // until it listens, or the alarm
  }

  uint64_t cmd = 0, after = 0;
  int rc = rpc(fd, CMD_CFG_READ, PCI_COMMAND, 2, 0, &cmd);
  if (rc == -1) {
    say("the server did not answer the first read in %.1f s: it is serving another client or it is wedged; nothing written", reply_timeout);
    return 4;
  }
  if (rc) { say("the first read failed; nothing written"); return 5; }
  if (cmd == 0xffffffffu || (cmd & 0xffff) == 0xffff) {
    say("PCI_COMMAND reads %#llx: the device is not there; nothing written", (unsigned long long)cmd);
    return 6;
  }
  if (cmd & PCI_COMMAND_MASTER) {
    if (rpc(fd, CMD_CFG_WRITE, PCI_COMMAND, 2, cmd & ~(uint64_t)PCI_COMMAND_MASTER, NULL)) { say("the write failed"); return 5; }
  }
  if (rpc(fd, CMD_CFG_READ, PCI_COMMAND, 2, 0, &after)) { say("the read-back failed"); return 5; }
  say("PCI_COMMAND %#06llx -> %#06llx: bus mastering is %s", (unsigned long long)cmd, (unsigned long long)after,
      after & PCI_COMMAND_MASTER ? "STILL ON" : "off");
  close(fd);
  return after & PCI_COMMAND_MASTER ? 7 : 0;
}

#ifdef __APPLE__
// One clear, in a child of its own with its own alarm: the hard timeout then ends that attempt and not the watchdog,
// which may still have the client to wait for.
static int watched_clear(const char *sock, int old_pid, int old_gone, const char *app, double timeout, double reply_timeout,
                         int no_start) {
  pid_t pid = fork();
  if (pid == 0) {
    signal(SIGALRM, on_alarm);
    alarm((unsigned)(timeout + 0.999));
    _exit(clear(sock, old_pid, old_gone, app, reply_timeout, no_start));
  }
  if (pid < 0) { say("could not fork the clear: %s", strerror(errno)); return -1; }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  say("the clear exited %d%s", rc, rc ? ": bus mastering was NOT confirmed off" : "");
  return rc;
}

static int watch(int server_pid, const char *app, double timeout, double reply_timeout, int no_start) {
  signal(SIGINT, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  g_patient = 1;
  const char *sock = getenv("TINYNV_BMCLEAR_SOCK");
  if (!sock || !*sock) { fprintf(stderr, "tinynv-bmclear: --watch needs TINYNV_BMCLEAR_SOCK; the socket is never guessed\n"); return 2; }
  // The client is its pipe, not a pid. Every process holding the session holds the pipe too (pci_tinygpu.c: the
  // socket and the pipe's write end are both close-on-exec and both pass through a fork), so end of file on fd 3
  // means no process holds the session any more. A pid said less: a client that forked and exited left its child
  // in the session, and a watchdog firing on the parent's exit sat in the listen(1) backlog against a live session
  // and cost the next program its connect (found by review, 2026-11-19). The pid is only for the log.
  int client = (int)getppid(), kq = kqueue(), armed = 0, ever = 0, cleared = 0, server_watched = 0;
  fcntl(3, F_SETFL, fcntl(3, F_GETFL) | O_NONBLOCK);
  struct kevent ev[1];
  EV_SET(&ev[0], 3, EVFILT_READ, EV_ADD, 0, 0, NULL);
  if (kq < 0 || kevent(kq, ev, 1, NULL, 0, NULL)) { say("watch: kqueue: %s; not watching", strerror(errno)); return 5; }
  if (server_pid > 0) {
    EV_SET(&ev[0], server_pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (!kevent(kq, ev, 1, NULL, 0, NULL)) server_watched = 1;
    else if (errno != ESRCH) say("watch: cannot watch server pid %d (%s); watching the client only", server_pid, strerror(errno));
  }
  say("watching client pid %d and server pid %d%s on %s", client, server_pid, server_pid > 0 && !server_watched ? " (already gone)" : "", sock);
  int server_gone = server_pid > 0 && !server_watched, client_gone = 0, said_idle = 0, tries = 0;
  double next_try = 0, gap = 5;   // a failed server-death clear is tried again after `gap` seconds, doubling to 80
  for (;;) {
    if (client_gone) {
      if (cleared) say("client pid %d gone; bus mastering was cleared once already, after server pid %d exited: done", client, server_pid);
      else if (!armed) say("stood down: clean - client pid %d %s", client, ever ? "read bus mastering back off" : "never turned bus mastering on");
      else {
        say("client exited without a clean teardown (pid %d): clearing bus mastering", client);
        g_why = "the watchdog saw the client exit without a clean teardown (M4)";
        return watched_clear(sock, server_pid, server_gone != 0, app, timeout, reply_timeout, no_start);
      }
      return 0;
    }
    // THE SERVER GONE UNDER A LIVE, ARMED CLIENT: cleared, and if that fails, again - every 5 s, then 10, 20, 40, 80 -
    // for as long as the client lives and stays armed, not only when it dies. An idle client can live for hours, and
    // the first clear fails whenever the app it starts needs more than the hard timeout to listen (round 3 of the
    // review). Only a clear that confirmed bus mastering off counts; 'C' or end of file end the retries (the latter
    // with the clear above). One at a time: watched_clear returns only when its child is gone, and other helpers are
    // kept out by the lock, under which nobody starts a server beside one that lives.
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    double t = (double)mono.tv_sec + (double)mono.tv_nsec * 1e-9;
    int due = server_gone && !cleared && armed;
    if (server_gone && !cleared && !armed && !said_idle++)
      say("server pid %d exited; this client has bus mastering off or never turned it on: nothing to do", server_pid);
    if (due && t >= next_try) {
      if (!tries++) say("server pid %d exited while client pid %d is alive: clearing bus mastering", server_pid, client);
      else say("server pid %d is gone and bus mastering is not confirmed off: clearing again (attempt %d)", server_pid, tries);
      g_why = "the watchdog saw the old server exit under a live client (M4)";
      cleared = watched_clear(sock, server_pid, 1, app, timeout, reply_timeout, no_start) == 0;
      if (cleared) continue;
      say("trying again in %.0f s, unless the client says clean or goes first", gap);
      clock_gettime(CLOCK_MONOTONIC, &mono);
      next_try = (double)mono.tv_sec + (double)mono.tv_nsec * 1e-9 + gap;
      if (gap < 80) gap *= 2;
      continue;   // the time has moved on: work out the wait afresh
    }
    struct timespec wait = {0, 0};
    if (due) { wait.tv_sec = (time_t)(next_try - t); wait.tv_nsec = (long)((next_try - t - (double)wait.tv_sec) * 1e9); }
    struct kevent got[4];
    int k = kevent(kq, NULL, 0, got, 4, due ? &wait : NULL);
    if (k < 0) { if (errno == EINTR) continue; say("watch: kevent: %s; stopping", strerror(errno)); return 5; }
    for (int i = 0; i < k; i++) {
      if (got[i].filter == EVFILT_PROC && (int)got[i].ident == server_pid) { server_gone = 1; continue; }
      // every byte in the pipe; end of file is the last holder of the session gone
      char b[64];
      ssize_t r;
      while ((r = read(3, b, sizeof(b))) > 0)
        for (ssize_t j = 0; j < r; j++) {
          // the last byte decides, whoever in the session sent it; nothing else - the client's own rescue having
          // started least of all - makes a session clean
          if (b[j] == 'A') armed = ever = 1;
          else if (b[j] == 'C') armed = 0;
        }
      if (r == 0) client_gone = 1;
    }
  }
}
#endif

int main(int argc, char **argv) {
  const char *sock = NULL, *app = getenv("TINYGPU_APP");
  int old_pid = 0, no_start = 0, watching = 0;
  double timeout = 0, reply_timeout = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--no-start")) { no_start = 1; continue; }
    if (!strcmp(a, "--watch")) { watching = 1; continue; }
    if (!strcmp(a, "--paths")) {   // where a server started now would be started through and log to; the only side effect is creating the log directory when the fallback ~/Library/Logs/macuda is chosen
      char sp[PATH_MAX], lp[PATH_MAX];
      server_script(sp, sizeof(sp));
      server_log(lp, sizeof(lp));
      printf("script %s\nlog %s\n", sp, lp);
      return 0;
    }
    if (!v) { fprintf(stderr, "tinynv-bmclear: %s needs a value\n", a); return 2; }
    if (!strcmp(a, "--sock")) sock = v;
    else if (!strcmp(a, "--old-pid")) old_pid = atoi(v);
    else if (!strcmp(a, "--app")) app = v;
    else if (!strcmp(a, "--timeout")) timeout = atof(v);
    else if (!strcmp(a, "--reply-timeout")) reply_timeout = atof(v);
    else { fprintf(stderr, "tinynv-bmclear: unknown argument %s\n", a); return 2; }
    i++;
  }
  if (timeout == 0) timeout = watching ? 10.0 : 5.0;
  if (reply_timeout == 0) reply_timeout = watching ? 8.0 : 1.0;
  if (!app || !*app) app = DEFAULT_APP;
  signal(SIGPIPE, SIG_IGN);
  if (watching) {
#ifdef __APPLE__
    if (sock || timeout < 0 || reply_timeout < 0) { fprintf(stderr, "tinynv-bmclear: --watch takes the socket from TINYNV_BMCLEAR_SOCK, never --sock\n"); return 2; }
    return watch(old_pid, app, timeout, reply_timeout, no_start);
#else
    fprintf(stderr, "tinynv-bmclear: --watch is macOS only (kqueue)\n");
    return 2;
#endif
  }
  // No default socket: which server to reach is the caller's to say, never guessed from this process's environment.
  if (!sock || !*sock || timeout <= 0 || reply_timeout <= 0) {
    fprintf(stderr, "usage: tinynv-bmclear --sock PATH [--old-pid N] [--app PATH] [--timeout S] [--reply-timeout S] [--no-start]\n"
                    "       tinynv-bmclear --watch [--old-pid N] [--app PATH] [--timeout S] [--reply-timeout S] [--no-start]\n");
    return 2;
  }
  signal(SIGALRM, on_alarm);
  alarm((unsigned)(timeout + 0.999));
  return clear(sock, old_pid, 0, app, reply_timeout, no_start);
}

// The out-of-process watchdog (M4 of the 2026-11-18 DART panic analysis), with real processes and real signals and no
// card. M2 and M3 only help a client that is alive to notice its server is gone; two ways out were still unguarded:
// the client killed (or crashed) under a live server, which then tears the session down with bus mastering on, and the
// server dying under an idle client, which nobody notices because nobody touches the device. The step-B bug slipped
// past tests that ignored SIGPIPE and half-closed a socket, so here every death is a SIGKILL of a real process and the
// client keeps SIGPIPE at its default.
//
// One binary, three roles, re-executed: the harness; `server SOCK`, the fake TinyGPU server (sequential, one client at
// a time, listen(1), reading each client to end of file before the next accept, logging every request) - which is
// ALSO the stand-in app the helper starts as `APP server SOCK`, so every start is counted; and `client MODE SOCK`, a
// libtinynv client that opens the transport, turns bus mastering on and waits for SIGUSR1. The card is a file holding
// PCI_COMMAND, so it outlives a server as the real one does. Nothing here names $TMPDIR/tinygpu.sock or the installed
// app: the socket is in a fresh /tmp directory and TINYGPU_APP is this binary.
//
//   1 clean       quiesce, exit: the server sees ONE accept, the watchdog exits, nothing cleared
//   1b unarmed    a client that never turned bus mastering on exits: no connection either
//   2 kill -9     the client: within about 1 s a fresh connection reads, writes PCI_COMMAND without MASTER, reads back;
//                 the server sees the client's connection end at once (7), and the watchdog's argv has no socket (7)
//   3 killpg -9   the client's whole process group: the same, the watchdog is not in it
//   4 server -9   under an idle client: the stand-in app is started ONCE and the card cleared; the client's later
//                 death costs no second clear
//   5 together    the client's own rescue and the watchdog's server-death trigger: one app start; and two one-shot
//                 helpers against a dead server in the same instant: one start, both clear
//   R             the session lost with the server alive: the client's own rescue clears, and the watchdog - which no
//                 longer stands down for a rescue that has only STARTED - looks once the client has gone, finds bus
//                 mastering off and writes nothing
//   R slow        the same loss under a server 3 s in the dead session's teardown: the rescue's 1 s reply timeout
//                 expires (exit 4), and the watchdog, still there, clears (round 2 of the review, 2026-11-19)
//   no stdio      the client's fd 2 closed, and fds 0-2 closed (a daemon, `2>&-`): socket() and pipe() then hand out
//                 0-2, and the watchdog once got the client's own server socket, or the pipe's write end, as its
//                 stdout and stderr. kill -9 clears within about 1 s all the same, the session ends at once, and
//                 the server reads nothing on the client's connection that is not a request - not the watchdog's
//                 log, not what the client itself prints to a closed fd 2. And the rescue (M3) starts and clears.
//   slow app      the server dies under an idle armed client and the app the watchdog starts takes 12 s to listen, so
//                 that clear times out (124). (a) the client killed meanwhile: the last clear finds no socket file
//                 (ENOENT, start_server unlinked it) and once exited 3 with the card on - it waits for the server
//                 that is coming up. (b) the client idle and alive: the clear is tried again and lands with nobody
//                 dying; one app start in both (round 3 of the review)
//   sticky        a quiesce whose read-back still has MASTER says nothing, and the watchdog clears after the exit
//   no fds        the descriptor table full (EMFILE) with stdio closed, so the session's descriptors cannot be moved
//                 off 0-2: a socket left there is not used (the open fails), a pipe end left there means no watchdog
//   refork        armed, forked, the child quiesced and exited, the parent turned bus mastering on again and was
//                 killed: cleared. The watchdog goes by the LAST byte, so every arming has to send one.
//   6 off        TINYNV_WATCHDOG=0, TINYNV_EMERGENCY_BM=0: no watchdog process, no clear after kill -9
//   8 slow        the server spends 3 s tearing the dead session down: the clear still lands, inside the hard timeout
//   exit          an armed client that exits without its teardown (TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL=1's path): cleared
//   sigpipe       the watchdog killed first, then a clean teardown with SIGPIPE at default: the client exits 0
//   fork          the client forks and the parent exits: the child still holds the session, so nothing fires while
//                 it lives and quiesces - and its kill -9 does fire (the pipe, not a pid, says the session is gone)
//   exec          the client exec'd a child and was killed: the session still ends at once, and is cleared
//   signals       SIGINT, SIGTERM and SIGHUP to the watchdog itself: it lives, and still clears on the client's death
//   started app   (in 4) answers SIGTERM, logs to TINYGPU_SERVER_LOG with a line saying who started it, and does not
//                 hold the client's stderr: the client's stderr is a pipe here and reaches end of file once the client
//                 and the watchdog are gone, with the app still running
//   spellings     (in 5) two helpers at once, /tmp/X and /private/tmp/X: one lock, one start
//   through the script  (M6 for the helper) the app a helper starts runs as tools/tinygpu-server.sh starts it: under
//                 script(1), from `sh SCRIPT start`. kill -9 it, and the server log has "server exit status 137" and
//                 EGPU_ROOT/logs both DART snapshots - written by the script's subshell after the client, the watchdog
//                 and the helper have all gone. It ignores SIGHUP, holds neither the lock nor the client's stderr; and
//                 4, 5 and slow app run again with the script, one start each, the started app stopped by SIGTERM
//   unreadable    the script there but unreadable: the direct spawn, one start, and the log says it fell back
// The script is given as the second argument and switched on only for those cases; every other case has
// TINYGPU_SERVER_SCRIPT at a path that does not exist (the direct spawn), and EGPU_ROOT is always a temp directory of
// this test's, so the script cannot write the repository's logs/.
#include "tinynv_pci.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_CFG_READ = 3, CMD_CFG_WRITE = 4 };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static void nap(double s) { struct timespec t = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)}; nanosleep(&t, NULL); }

// One line per event, one write() each, so the processes sharing the log never interleave inside a line.
static void logev(const char *fmt, ...) {
  const char *path = getenv("WD_LOG");
  if (!path) return;
  char line[256];
  int n = snprintf(line, sizeof(line), "%.6f %d ", now(), (int)getpid());
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
  va_end(ap);
  line[n++] = '\n';
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
  if (fd >= 0) { if (write(fd, line, (size_t)n) < 0) {} close(fd); }
}

static uint32_t card_get(void) {
  uint32_t v = 0xffffffffu;
  FILE *f = fopen(getenv("WD_CARD"), "r");
  if (f) { if (fscanf(f, "%x", &v) != 1) v = 0xffffffffu; fclose(f); }
  return v;
}
static void card_set(uint32_t v) {
  FILE *f = fopen(getenv("WD_CARD"), "w");
  if (f) { fprintf(f, "%#x\n", v); fclose(f); }
}

// ---- the fake server, and the stand-in app ----
static int role_server(const char *sock) {
  signal(SIGPIPE, SIG_IGN);
  logev("start");
  const char *td = getenv("WD_TEARDOWN_MS"), *drop = getenv("WD_DROP_AFTER"), *slow = getenv("WD_APP_DELAY_MS");
  // WD_APP_DELAY_MS, set by a case only after its own server is up: an app slow to listen (the real one opens the
  // device first). WD_STICKY: the first connection's writes that take MASTER off change nothing - a card that keeps it set.
  int sticky = getenv("WD_STICKY") != NULL;
  if (slow) nap(atof(slow) / 1000.0);
  double teardown = td ? atof(td) / 1000.0 : 0;
  int l = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock);
  unlink(sock);
  if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1)) { perror("fake server: listen"); return 2; }
  for (int conn = 1;; conn++) {
    int s = accept(l, NULL, NULL);
    if (s < 0) { if (errno == EINTR) { conn--; continue; } return 2; }
    logev("accept %d", conn);
    req_t rq;
    ssize_t got = 0;
    // WD_DROP_AFTER=N: the first connection is dropped after N requests and the server stays up - a lost session the
    // client finds by itself, so its own rescue (M3) runs and the watchdog has to stand down
    for (int nreq = 0; !(conn == 1 && drop && nreq == atoi(drop)) && (got = recv(s, &rq, sizeof(rq), MSG_WAITALL)) == (ssize_t)sizeof(rq); nreq++) {
      resp_t rp = {0};
      // a request is 33 bytes with a command the protocol has (0..12) for device 0; anything else is somebody's text
      if (rq.cmd > 12 || rq.dev_id) logev("bad %d %u", conn, rq.cmd);
      else if (rq.cmd == CMD_CFG_READ && rq.arg0 == 4) { rp.resp0 = card_get(); logev("read %d %#llx", conn, (unsigned long long)rp.resp0); }
      else if (rq.cmd == CMD_CFG_WRITE && rq.arg0 == 4) { if (!(sticky && conn == 1 && !(rq.arg2 & 4))) card_set((uint32_t)rq.arg2); logev("write %d %#llx", conn, (unsigned long long)rq.arg2); }
      else logev("other %d %u", conn, rq.cmd);
      if (send(s, &rp, sizeof(rp), 0) < 0) break;
    }
    if (got > 0 && got != (ssize_t)sizeof(rq)) logev("bad %d %d", conn, (int)got);   // a tail that is no request
    logev("eof %d", conn);
    close(s);
    if (teardown > 0) nap(teardown);   // the dead session's teardown, during which nothing is accepted
  }
}

// ---- the client ----
static int role_client(const char *mode, const char *sock) {
  signal(SIGPIPE, SIG_DFL);
  setpgid(0, 0);
  sigset_t us;
  sigemptyset(&us);
  sigaddset(&us, SIGUSR1);
  sigprocmask(SIG_BLOCK, &us, NULL);
  // A real program has files open before the device. Here the socket would otherwise land on fd 3 - where the spawn
  // puts the watchdog's pipe, overwriting it - and an inherited socket (no CLOEXEC_DEFAULT) would go unseen.
  for (int i = 0; i < 4; i++) if (open("/dev/null", O_RDONLY) < 0) return 2;
  // WD_CLOSE: a daemon's descriptors. "2" closes stderr (`2>&-`), "012" all three - AFTER the opens above, so the
  // lowest free descriptors, which socket() and pipe() take, are 0-2.
  const char *cl = getenv("WD_CLOSE");
  for (; cl && *cl; cl++) close(*cl - '0');
  // WD_FILL: no descriptor to move to. The table is 64 and full but for the ones WD_CLOSE closed - and, for "two", two
  // more above them, which the socket and the pipe's read end take, leaving the write end on 0-2 with nowhere to go.
  // One filler is closed again after the open, so the rest of this client (and its event log) has a descriptor.
  const char *fill = getenv("WD_FILL");
  int filler[64], nf = 0, f;
  if (fill) {
    struct rlimit rl = {64, 64};
    setrlimit(RLIMIT_NOFILE, &rl);
    while ((f = open("/dev/null", O_RDONLY)) >= 0) filler[nf++] = f;
    for (int i = 0; i < nf; i++) if (filler[i] <= 2) close(filler[i]);   // the closed stdio, taken by the loop above
    if (!strcmp(fill, "two")) { close(filler[--nf]); close(filler[--nf]); }
  }
  tinynv_pci_t p;
  int failed = tinynv_pci_open_tinygpu(sock, &p);
  if (fill) close(filler[nf - 1]);
  if (failed) { logev("openfail"); fprintf(stderr, "client: open: %s\n", tinynv_last_error()); return 2; }
  uint32_t cmd = p.cfg_read(&p, 4, 2);
  if (strcmp(mode, "unarmed")) p.cfg_write(&p, 4, 2, cmd | 4);
  // what any program (and libtinynv's own fprintf) does with a closed fd 2: write to it anyway
  // - and to a closed stdout: with the pipe's write end there, the 'C' in this line was the watchdog's last byte
  if (getenv("WD_CLOSE")) {
    static const char m[] = "Client: armed, not Clean - a line for stderr, which is closed\n", o[] = "Client: armed, not Clean - a line for stdout\n";
    if (write(2, m, sizeof(m) - 1) < 0) {}
    if (strchr(getenv("WD_CLOSE"), '1') && write(1, o, sizeof(o) - 1) < 0) {}
  }
  logev("armed %s", mode);
  int sig;
  sigwait(&us, &sig);
  if (!strcmp(mode, "clean")) { p.quiesce(&p); logev("quiesced"); return 0; }
  if (!strncmp(mode, "fork", 4)) {
    // the parent goes, the child keeps the session: its socket and the watchdog's pipe both
    if (fork()) _exit(0);
    logev("child");
    if (!strcmp(mode, "fork-clean")) { nap(2); p.quiesce(&p); logev("quiesced"); _exit(0); }
    for (;;) pause();
  }
  if (!strcmp(mode, "exec")) {
    // a program that starts a helper of its own and is then killed: the helper must hold neither the session nor the
    // pipe, or the server would not see the session end while the watchdog, seeing the pipe close, fired against it
    pid_t c = fork();
    if (!c) { execl("/bin/sleep", "sleep", "4", (char *)NULL); _exit(127); }
    logev("child %d", (int)c);
    for (;;) pause();
  }
  if (!strncmp(mode, "rescue", 6)) {
    p.cfg_read(&p, 4, 2);   // the server is gone: this latches the loss
    p.rescue(&p, 9.0);
    logev("rescued");
    if (!strcmp(mode, "rescue-stay")) for (;;) pause();   // what the rescue did, seen with the client still alive
    return 0;
  }
  if (!strcmp(mode, "refork")) {
    // armed above; a child quiesces and goes; this process turns bus mastering on again
    pid_t c = fork();
    if (!c) { p.quiesce(&p); _exit(0); }
    waitpid(c, NULL, 0);
    logev("quiesced");
    cmd = p.cfg_read(&p, 4, 2);
    p.cfg_write(&p, 4, 2, cmd | 4);
    p.cfg_read(&p, 4, 2);   // a round trip: the write has landed
    logev("rearmed");
    for (;;) pause();
  }
  return 0;   // "exit", "unarmed": out without a teardown
}

// ---- the harness ----
static const char *helper, *script;
static char self[PATH_MAX], dir[64], sock[128], logp[160], card[160], errp[160], slog[160], root[64];
static int script_is_on;
// the helper starts its server through tools/tinygpu-server.sh, or (a path that does not exist) directly
static void script_on(int on) {
  script_is_on = on;
  setenv("TINYGPU_SERVER_SCRIPT", on ? script : "/nonexistent/tools/tinygpu-server.sh", 1);
}
// The client's stderr is a pipe, as it is under a shell or tools/serve.sh: whoever still holds its write end keeps a
// reader from ever seeing end of file. drain() copies what has arrived into errp and says whether the end has come.
static int errfd = -1;
static int drain(void) {
  if (errfd < 0) return 1;
  char b[4096];
  ssize_t n;
  int f = open(errp, O_WRONLY | O_APPEND | O_CREAT, 0600);
  while ((n = read(errfd, b, sizeof(b))) > 0) if (f >= 0 && write(f, b, (size_t)n) < 0) break;
  if (f >= 0) close(f);
  return n == 0;
}

typedef struct { double t; int pid; char ev[16]; int conn; unsigned long v; } ev_t;
static int load(ev_t *e, int max) {
  FILE *f = fopen(logp, "r");
  int n = 0;
  char line[256];
  if (!f) return 0;
  while (n < max && fgets(line, sizeof(line), f)) {
    ev_t x = {0};
    if (sscanf(line, "%lf %d %15s %d %lx", &x.t, &x.pid, x.ev, &x.conn, &x.v) >= 3) e[n++] = x;
  }
  fclose(f);
  return n;
}
// events named `ev` after time t0 (and from pid, when nonzero); the last one in *last
static int count(const char *ev, double t0, int pid, ev_t *last) {
  ev_t e[512];
  int n = load(e, 512), c = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(e[i].ev, ev) && e[i].t >= t0 && (!pid || e[i].pid == pid)) { c++; if (last) *last = e[i]; }
  return c;
}
// the first event named `ev` after t0 from pid, or 0
static int first(const char *ev, double t0, int pid, ev_t *out) {
  ev_t e[512];
  int n = load(e, 512);
  for (int i = 0; i < n; i++)
    if (!strcmp(e[i].ev, ev) && e[i].t >= t0 && (!pid || e[i].pid == pid)) { *out = e[i]; return 1; }
  return 0;
}
static int wait_for(const char *ev, double t0, int pid, int n, double limit, ev_t *last) {
  double end = now() + limit;
  int c;
  while ((c = count(ev, t0, pid, last)) < n && now() < end) nap(0.02);
  return c;
}

static void case_env(const char *teardown_ms) {
  snprintf(dir, sizeof(dir), "/tmp/tgwd.XXXXXX");
  if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
  snprintf(sock, sizeof(sock), "%s/tinygpu.sock", dir);
  snprintf(logp, sizeof(logp), "%s/events", dir);
  snprintf(card, sizeof(card), "%s/card", dir);
  snprintf(errp, sizeof(errp), "%s/client.err", dir);
  snprintf(slog, sizeof(slog), "%s/server.log", dir);
  setenv("TINYGPU_SERVER_LOG", slog, 1);
  setenv("WD_LOG", logp, 1);
  setenv("WD_CARD", card, 1);
  if (teardown_ms) setenv("WD_TEARDOWN_MS", teardown_ms, 1); else unsetenv("WD_TEARDOWN_MS");
  unsetenv("WD_DROP_AFTER");
  unsetenv("WD_CLOSE");
  unsetenv("WD_FILL");
  unsetenv("WD_APP_DELAY_MS");
  unsetenv("WD_STICKY");
  card_set(0x0403);
}
static pid_t spawn_server(void) {
  char *argv[] = {self, "server", sock, NULL};
  pid_t pid;
  if (posix_spawn(&pid, self, NULL, NULL, argv, environ)) { perror("spawn server"); exit(2); }
  double end = now() + 3;
  struct stat st;
  while (stat(sock, &st) && now() < end) nap(0.01);   // until it listens
  nap(0.02);
  return pid;
}
static pid_t spawn_client(const char *mode) {
  char *argv[] = {self, "client", (char *)mode, sock, NULL};
  posix_spawn_file_actions_t fa;
  int pp[2];
  if (pipe(pp)) { perror("pipe"); exit(2); }
  fcntl(pp[0], F_SETFD, FD_CLOEXEC);
  fcntl(pp[1], F_SETFD, FD_CLOEXEC);
  fcntl(pp[0], F_SETFL, O_NONBLOCK);
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, pp[1], 2);
  pid_t pid;
  if (posix_spawn(&pid, self, &fa, NULL, argv, environ)) { perror("spawn client"); exit(2); }
  posix_spawn_file_actions_destroy(&fa);
  close(pp[1]);
  if (errfd >= 0) close(errfd);
  errfd = pp[0];
  for (double end = now() + 3; !count("armed", 0, pid, NULL) && !count("openfail", 0, pid, NULL) && now() < end;) nap(0.02);
  if (!count("armed", 0, pid, NULL) && !count("openfail", 0, pid, NULL)) printf("note: client %d (%s) did not arm\n", (int)pid, mode);
  return pid;
}
// the children of pid (pgrep -P): the count, and the first
static int children(pid_t pid, pid_t *first) {
  char c[64];
  snprintf(c, sizeof(c), "pgrep -P %d", (int)pid);
  FILE *f = popen(c, "r");
  int n = 0, x;
  if (first) *first = 0;
  while (f && fscanf(f, "%d", &x) == 1) { if (!n && first) *first = x; n++; }
  if (f) pclose(f);
  return n;
}
static int args_of(pid_t pid, char *out, size_t n) {
  char c[64];
  snprintf(c, sizeof(c), "ps -o args= -p %d", (int)pid);
  FILE *f = popen(c, "r");
  out[0] = 0;
  if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; pclose(f); }
  return out[0] != 0;
}
static int ps_int(pid_t pid, const char *field) {
  char c[64];
  int v = 0;
  snprintf(c, sizeof(c), "ps -o %s= -p %d", field, (int)pid);
  FILE *f = popen(c, "r");
  if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; pclose(f); }
  return v;
}
// An app started through the script: its parent is the `sh -c` inside script(1), whose parent is script(1), whose
// parent is the script's own subshell. 0 when pid is not such an app; else the subshell's pid, and script(1)'s in *sc.
static pid_t wrapper_of(pid_t app, pid_t *sc) {
  char a[1024];
  pid_t sh = ps_int(app, "ppid"), s = sh > 1 ? ps_int(sh, "ppid") : 0, sub = s > 1 ? ps_int(s, "ppid") : 0;
  if (sc) *sc = s;
  if (!sh || !args_of(sh, a, sizeof(a)) || strncmp(a, "/bin/sh -c", 10)) return 0;
  if (!s || !args_of(s, a, sizeof(a)) || strncmp(a, "script ", 7)) return 0;
  if (!sub || !args_of(sub, a, sizeof(a)) || !strstr(a, script) || !strstr(a, " start")) return 0;
  return sub;
}
// The script's subshell, script(1) and whatever they run (sleep, ioreg) share one process group: gone at once, so a
// case that is not about the snapshots leaves nothing behind writing into its directory.
static void unwrap(pid_t app) {
  pid_t sc, sub = wrapper_of(app, &sc);
  int g = sub ? ps_int(sub, "pgid") : 0;
  if (g > 1 && g != getpgrp()) kill(-g, SIGKILL);
}
// alive and not a zombie: the watchdog is the client's child, and kill(pid, 0) succeeds on one it has not reaped
static int running(pid_t pid) {
  char c[64], st[16] = "";
  snprintf(c, sizeof(c), "ps -o stat= -p %d", (int)pid);
  FILE *f = popen(c, "r");
  if (f) { if (!fgets(st, sizeof(st), f)) st[0] = 0; pclose(f); }
  return st[0] && st[0] != 'Z';
}
static int gone(pid_t pid, double limit) {
  double end = now() + limit;
  while (now() < end) { if (kill(pid, 0) && errno == ESRCH) return 1; nap(0.02); }
  return kill(pid, 0) && errno == ESRCH;
}
static int reap(pid_t pid, double limit) {
  int st;
  double end = now() + limit;
  while (waitpid(pid, &st, WNOHANG) == 0) {
    if (now() > end) { kill(pid, SIGKILL); waitpid(pid, &st, 0); return -1; }
    nap(0.01);
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static int err_eof(double limit) {
  double end = now() + limit;
  while (!drain() && now() < end) nap(0.02);
  return drain();
}
static int file_has(const char *path, const char *needle) {
  if (path == errp) drain();
  FILE *f = fopen(path, "r");
  char line[8192];   // whole lines: the helper's "started ... through SCRIPT" line carries two full paths
  int found = 0;
  if (!f) return 0;
  while (!found && fgets(line, sizeof(line), f)) found = strstr(line, needle) != NULL;
  fclose(f);
  return found;
}
// Every stand-in this case started (its "start" lines), the watchdog if it is still up - all of them processes this
// test made - and the directory.
static void case_end(pid_t server, pid_t wd) {
  if (wd > 0 && !gone(wd, 12)) { CHECK(0, "the watchdog %d is still running at the end of the case", (int)wd); kill(wd, SIGKILL); }
  ev_t e[512];
  int n = load(e, 512);
  for (int i = 0; i < n; i++) if (!strcmp(e[i].ev, "start") && e[i].pid != server) { unwrap(e[i].pid); kill(e[i].pid, SIGKILL); }
  if (server > 0) { kill(server, SIGKILL); waitpid(server, NULL, 0); }
  drain();
  if (errfd >= 0) { close(errfd); errfd = -1; }
  char lock[200];
  snprintf(lock, sizeof(lock), "%s.bmclear.lock", sock);
  unlink(lock); unlink(sock); unlink(logp); unlink(card); unlink(errp); unlink(slog);
  rmdir(dir);
}
static void show_err(const char *what) {
  static int shown;
  if (fails == shown) return;   // only for a case that has just failed
  shown = fails;
  drain();
  printf("--- %s: the client's stderr (%s)\n", what, errp);
  FILE *f = fopen(errp, "r");
  char line[512];
  while (f && fgets(line, sizeof(line), f)) printf("    %s", line);
  if (f) fclose(f);
}

// 2, 3, 7: the client killed (or its group), the server alive
static void case_killed(int group) {
  const char *name = group ? "killpg -9" : "kill -9";
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  int nwd = children(client, &wd);
  CHECK(nwd == 1, "%s: the client has %d children, want the one watchdog", name, nwd);
  if (!group) {
    char args[1024];
    CHECK(wd && args_of(wd, args, sizeof(args)) && !strstr(args, sock) && !strstr(args, "tinygpu.sock"),
          "kill -9: the watchdog's argv names the socket (pkill -f tinygpu.sock would take it): %s", wd ? args : "(none)");
  }
  double t0 = now();
  if (group) kill(-client, SIGKILL); else kill(client, SIGKILL);
  reap(client, 3);
  ev_t eof, w, rb;
  wait_for("eof", t0, server, 1, 0.5, NULL);
  CHECK(first("eof", t0, server, &eof) && eof.conn == 1 && eof.t - t0 < 0.5,
        "%s: the server did not see the client's connection end within 0.5 s (the watchdog holds it?)", name);
  int writes = wait_for("write", t0, server, 1, 3, &w);
  CHECK(writes == 1 && w.v == 0x0403 && w.t - t0 < 1.5, "%s: %d write(s) after the kill, want one of 0x0403 within about 1 s (got %#lx after %.2f s)",
        name, writes, writes ? w.v : 0, writes ? w.t - t0 : -1.0);
  nap(0.2);
  CHECK(count("read", writes ? w.t : t0, server, &rb) == 1 && rb.v == 0x0403, "%s: the write was not read back", name);
  CHECK(card_get() == 0x0403, "%s: the card ends at %#x, want 0x0403", name, card_get());
  CHECK(count("accept", t0, server, NULL) == 1, "%s: %d connections after the kill, want the watchdog's one", name, count("accept", t0, server, NULL));
  CHECK(count("start", 0, 0, NULL) == 1, "%s: an app was started under a live server", name);
  CHECK(file_has(errp, "client exited without a clean teardown"), "%s: the watchdog did not say which trigger fired", name);
  if (fails) show_err(name);
  case_end(server, wd);
}

static void case_clean(const char *mode) {
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client(mode), wd = 0;
  int nwd = children(client, &wd);
  CHECK(nwd == 1, "%s: the client has %d children, want the one watchdog", mode, nwd);
  double t0 = now();
  kill(client, SIGUSR1);
  int rc = reap(client, 3);
  CHECK(rc == 0, "%s: the client exited %d", mode, rc);
  CHECK(wd && gone(wd, 2), "%s: the watchdog did not exit after a clean teardown", mode);
  nap(0.3);
  CHECK(count("accept", 0, server, NULL) == 1, "%s: the server saw %d accepts, want the client's ONE", mode, count("accept", 0, server, NULL));
  CHECK(count("write", t0, 0, NULL) == (strcmp(mode, "clean") ? 0 : 1), "%s: something was written after the teardown", mode);
  CHECK(!file_has(errp, "connect") && !file_has(errp, "clearing bus mastering"), "%s: the watchdog went for the server", mode);
  if (!strcmp(mode, "clean")) CHECK(file_has(errp, "stood down: clean"), "clean: the watchdog did not say it stood down");
  case_end(server, wd);
}

// 4: the server killed under an idle client
static void case_server_dies(void) {
  case_env(NULL);
  if (script_is_on) printf("  (4 again, through the script)\n");
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(server, SIGKILL);
  waitpid(server, NULL, 0);
  ev_t st, w;
  int starts = wait_for("start", t0, 0, 1, 4, &st);
  int writes = wait_for("write", t0, 0, 1, 4, &w);
  CHECK(starts == 1, "server -9: the stand-in app was started %d times, want once", starts);
  CHECK(writes == 1 && w.v == 0x0403 && w.pid == st.pid, "server -9: %d write(s), want 0x0403 through the new server (got %#lx)", writes, writes ? w.v : 0);
  char want[64];
  snprintf(want, sizeof(want), "server pid %d exited", (int)server);
  CHECK(file_has(errp, want), "server -9: the watchdog did not say \"%s\"", want);
  // and the client's death afterwards: one clear per watchdog lifetime
  double t1 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  CHECK(wd && gone(wd, 3), "server -9: the watchdog did not exit after the client's death");
  CHECK(count("accept", t1, 0, NULL) == 0 && count("start", t1, 0, NULL) == 0, "server -9: the client's death cost a second clear");
  // the app the watchdog started: still running, but not on the client's stderr; in its own log; stopped by SIGTERM
  CHECK(starts == 1 && kill(st.pid, 0) == 0, "server -9: the started app is not running");
  CHECK(err_eof(1.0), "server -9: the client's stderr never reached end of file - the started app holds it");
  snprintf(want, sizeof(want), "(pid %d)", (int)st.pid);
  if (script_is_on) {   // the helper knows the script's pid, not the app's: the line names the script
    CHECK(starts == 1 && wrapper_of(st.pid, NULL), "server -9: the started app %d does not run under script(1) from %s start", (int)st.pid, script);
    CHECK(file_has(slog, "started") && file_has(slog, script), "server -9: %s has no line saying the helper started the server through %s", slog, script);
    if (starts == 1) unwrap(st.pid);   // its records are case_script's; this is about the start and the signal
  } else
    CHECK(file_has(slog, "started") && file_has(slog, want), "server -9: %s has no line saying the helper started pid %d", slog, (int)st.pid);
  if (starts == 1) kill(st.pid, SIGTERM);
  CHECK(starts == 1 && gone(st.pid, 1.5), "server -9: the started app ignored SIGTERM (pkill -f tinygpu.sock could not stop it)");
  if (fails) show_err("server -9");
  case_end(0, wd);
}

// The DART snapshots in EGPU_ROOT/logs whose names end in `suffix` (".txt", ".txt.5s")
static int snaps(const char *suffix) {
  char d[128];
  snprintf(d, sizeof(d), "%s/logs", root);
  DIR *dp = opendir(d);
  struct dirent *e;
  int n = 0;
  size_t ls = strlen(suffix);
  while (dp && (e = readdir(dp))) {
    size_t l = strlen(e->d_name);
    if (!strncmp(e->d_name, "dart-at-server-exit-", 20) && l > ls && !strcmp(e->d_name + l - ls, suffix)) n++;
  }
  if (dp) closedir(dp);
  return n;
}

// with the script on, every app started since t0 runs under it - not the direct spawn, which would pass the rest too
static void via_script(const char *name, double t0) {
  if (!script_is_on) return;
  ev_t e[512];
  int n = load(e, 512);
  for (int i = 0; i < n; i++)
    if (!strcmp(e[i].ev, "start") && e[i].t >= t0) CHECK(wrapper_of(e[i].pid, NULL), "%s: the app %d was not started through %s", name, e[i].pid, script);
}

// through the script: a server the helper starts gets M6's records, kept by a wrapper that outlives the helper
static void case_script(void) {
  case_env(NULL);
  // the path baked into the helper (Makefile: TINYNV_SERVER_SCRIPT_DEFAULT), which wrapper_of() checks is `script`
  unsetenv("TINYGPU_SERVER_SCRIPT");
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(server, SIGKILL);
  waitpid(server, NULL, 0);
  ev_t st = {0}, w = {0};
  int starts = wait_for("start", t0, 0, 1, 5, &st), writes = wait_for("write", t0, 0, 1, 5, &w);
  CHECK(starts == 1 && writes == 1 && w.v == 0x0403 && w.pid == st.pid, "script: %d start(s) and %d write(s) after the server's death, want one each through the new server",
        starts, writes);
  pid_t sc = 0, sub = starts == 1 ? wrapper_of(st.pid, &sc) : 0;
  CHECK(sub, "script: the started app %d does not run under script(1) from `sh %s start`", (int)st.pid, script);
  CHECK(file_has(slog, script) && file_has(slog, "exit under a live client"), "script: %s has no line saying the helper started the server through the script, and why", slog);
  // everybody who could have started it gone: the client, its watchdog, and with it the watchdog's clear
  kill(client, SIGKILL);
  reap(client, 3);
  CHECK(wd && gone(wd, 3), "script: the watchdog did not exit after the client's death");
  CHECK(sub && running(sub) && running(sc), "script: the script's subshell %d or script(1) %d did not outlive the helper", (int)sub, (int)sc);
  // a terminal's hangup does not stop it; a kill does
  if (starts == 1) kill(st.pid, SIGHUP);
  nap(0.3);
  CHECK(starts == 1 && running(st.pid), "script: the started app died of SIGHUP");
  // one connection to the new server, the clear's: the script's own liveness probe - a connect, about 0.1 s after the
  // socket appears, into the listen(1) backlog - is off for the helper (TINYGPU_START_NOWAIT). Counted a second on.
  nap(0.7);
  CHECK(starts == 1 && count("accept", 0, st.pid, NULL) == 1, "script: the started server saw %d connections, want the clear's one", count("accept", 0, st.pid, NULL));
  char lock[200];
  snprintf(lock, sizeof(lock), "%s.bmclear.lock", sock);
  int lf = open(lock, O_RDWR | O_CLOEXEC);
  CHECK(lf >= 0 && flock(lf, LOCK_EX | LOCK_NB) == 0, "script: the lock %s is still held with the helpers gone - the server or its wrapper holds it", lock);
  if (lf >= 0) close(lf);
  CHECK(err_eof(1.0), "script: the client's stderr never reached end of file - the server or its wrapper holds it");
  int n0 = snaps(".txt"), n5 = snaps(".txt.5s");
  double t1 = now();
  if (starts == 1) kill(st.pid, SIGKILL);
  double end = now() + 3;
  while (!file_has(slog, "server exit status 137") && now() < end) nap(0.05);
  CHECK(file_has(slog, "server exit status 137"), "script: no \"server exit status 137\" in %s after kill -9", slog);
  CHECK(file_has(slog, "script(1) around the server exited"), "script: the subshell did not write script(1)'s status to %s", slog);
  for (end = now() + 12; (snaps(".txt") <= n0 || snaps(".txt.5s") <= n5) && now() < end;) nap(0.1);
  CHECK(snaps(".txt") == n0 + 1 && snaps(".txt.5s") == n5 + 1, "script: want one more dart-at-server-exit snapshot and one more .5s in %s/logs, have %d and %d (%.1f s)",
        root, snaps(".txt") - n0, snaps(".txt.5s") - n5, now() - t1);
  CHECK(sub && gone(sub, 2), "script: the subshell %d is still running after its last snapshot", (int)sub);
  show_err("script");
  case_end(0, wd);
  script_on(1);
}

// the script there but unreadable: the direct spawn, as before, once; and the logs say so
static void case_unreadable(void) {
  case_env(NULL);
  char unr[200];
  snprintf(unr, sizeof(unr), "%s/tinygpu-server.sh", dir);
  FILE *f = fopen(unr, "w");
  if (f) { fputs("exit 0\n", f); fclose(f); }
  chmod(unr, 0);
  setenv("TINYGPU_SERVER_SCRIPT", unr, 1);
  card_set(0x0407);
  pid_t dead = fork();
  if (!dead) _exit(0);
  waitpid(dead, NULL, 0);
  char dp[16];
  snprintf(dp, sizeof(dp), "%d", (int)dead);
  char *argv[] = {(char *)helper, "--sock", sock, "--old-pid", dp, "--app", self, NULL};
  pid_t h[2];
  for (int i = 0; i < 2; i++) if (posix_spawn(&h[i], helper, NULL, NULL, argv, environ)) { perror("spawn"); exit(2); }
  int r0 = reap(h[0], 8), r1 = reap(h[1], 8);
  ev_t st = {0};
  int starts = count("start", 0, 0, &st);
  CHECK(r0 == 0 && r1 == 0 && starts == 1 && card_get() == 0x0403, "unreadable script: helpers exited %d and %d, %d start(s), card %#x; want 0, 0, one, 0x0403",
        r0, r1, starts, card_get());
  CHECK(starts == 1 && !wrapper_of(st.pid, NULL), "unreadable script: the app runs under a wrapper");
  CHECK(file_has(slog, "fell back") && file_has(slog, unr), "unreadable script: %s does not say the helper fell back from %s", slog, unr);
  unlink(unr);
  case_end(0, 0);
}

// 5: the client's own rescue and the watchdog's server-death trigger, together
static void case_together(void) {
  case_env(NULL);
  if (script_is_on) printf("  (5 again, through the script)\n");
  pid_t server = spawn_server(), client = spawn_client("rescue"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(server, SIGKILL);
  kill(client, SIGUSR1);
  waitpid(server, NULL, 0);
  int rc = reap(client, 12);
  CHECK(rc == 0, "together: the client exited %d", rc);
  nap(0.3);
  CHECK(wd && gone(wd, 8), "together: the watchdog did not exit");
  int starts = count("start", t0, 0, NULL);
  CHECK(starts == 1, "together: the app was started %d times, want exactly once", starts);
  via_script("together", t0);
  CHECK(card_get() == 0x0403, "together: the card ends at %#x", card_get());
  if (fails) show_err("together");
  case_end(0, wd);

  // two one-shot helpers in the same instant against a dead server: the lock makes one start, and both clear
  case_env(NULL);
  card_set(0x0407);
  pid_t dead = fork();
  if (!dead) _exit(0);
  waitpid(dead, NULL, 0);
  char dp[16];
  snprintf(dp, sizeof(dp), "%d", (int)dead);
  char *argv[] = {(char *)helper, "--sock", sock, "--old-pid", dp, "--app", self, NULL};
  pid_t h[2];
  for (int i = 0; i < 2; i++) if (posix_spawn(&h[i], helper, NULL, NULL, argv, environ)) { perror("spawn"); exit(2); }
  int r0 = reap(h[0], 8), r1 = reap(h[1], 8);
  CHECK(r0 == 0 && r1 == 0, "two helpers at once: they exited %d and %d, want both 0", r0, r1);
  CHECK(count("start", 0, 0, NULL) == 1, "two helpers at once: the app was started %d times", count("start", 0, 0, NULL));
  via_script("two helpers at once", 0);
  CHECK(card_get() == 0x0403, "two helpers at once: the card ends at %#x", card_get());
  case_end(0, 0);

  // ...and the same, the two spelling the socket differently: /tmp is /private/tmp, and it must be one lock
  case_env(NULL);
  card_set(0x0407);
  char alt[160];
  snprintf(alt, sizeof(alt), "/private%s", sock);
  char *argv2[] = {(char *)helper, "--sock", alt, "--old-pid", dp, "--app", self, NULL};
  if (posix_spawn(&h[0], helper, NULL, NULL, argv, environ) || posix_spawn(&h[1], helper, NULL, NULL, argv2, environ)) { perror("spawn"); exit(2); }
  r0 = reap(h[0], 8), r1 = reap(h[1], 8);
  CHECK(r0 == 0 && r1 == 0, "two spellings at once: they exited %d and %d, want both 0", r0, r1);
  CHECK(count("start", 0, 0, NULL) == 1, "two spellings at once: the app was started %d times", count("start", 0, 0, NULL));
  via_script("two spellings at once", 0);
  case_end(0, 0);
}

// fork: the session outlives the process that opened it
static void case_fork(int killed) {
  const char *name = killed ? "fork, child killed" : "fork, child clean";
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client(killed ? "fork-kill" : "fork-clean"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  CHECK(reap(client, 3) == 0, "%s: the parent did not exit 0", name);
  ev_t ch;
  CHECK(wait_for("child", t0, 0, 1, 2, &ch) == 1, "%s: no child", name);
  if (!killed) {
    CHECK(wait_for("quiesced", t0, 0, 1, 5, NULL) == 1, "%s: the child did not quiesce", name);
    CHECK(wd && gone(wd, 2), "%s: the watchdog did not exit", name);
    CHECK(count("accept", t0, server, NULL) == 0, "%s: %d connection(s) while the child held the session", name, count("accept", t0, server, NULL));
    CHECK(file_has(errp, "stood down: clean"), "%s: the watchdog did not stand down clean", name);
  } else {
    nap(0.5);
    CHECK(count("accept", t0, server, NULL) == 0, "%s: the watchdog connected while the child held the session", name);
    double t1 = now();
    kill(ch.pid, SIGKILL);
    ev_t w;
    CHECK(wait_for("write", t1, server, 1, 3, &w) == 1 && w.v == 0x0403, "%s: bus mastering was not cleared after the child's kill", name);
    CHECK(wd && gone(wd, 3), "%s: the watchdog did not exit", name);
  }
  show_err(name);
  case_end(server, wd);
}

// exec: a child the client exec'd holds no part of the session
static void case_exec(void) {
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("exec"), wd = 0;
  children(client, &wd);
  kill(client, SIGUSR1);
  ev_t ch, eof;
  CHECK(wait_for("child", 0, client, 1, 2, &ch) == 1, "exec: no child");
  nap(0.2);
  double t0 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  wait_for("eof", t0, server, 1, 0.5, NULL);
  CHECK(first("eof", t0, server, &eof) && eof.conn == 1 && eof.t - t0 < 0.5,
        "exec: the server did not see the session end within 0.5 s - the exec'd child holds the socket");
  ev_t w;
  CHECK(wait_for("write", t0, server, 1, 3, &w) == 1 && w.v == 0x0403 && w.t - t0 < 1.5, "exec: no clear within about 1 s");
  if (ch.conn > 0) kill(ch.conn, SIGKILL);   // the sleep this test's client started
  show_err("exec");
  case_end(server, wd);
}

// the watchdog shrugs off the signals a terminal or a stop script sends
static void case_signals(void) {
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  children(client, &wd);
  if (wd > 0) { kill(wd, SIGINT); kill(wd, SIGTERM); kill(wd, SIGHUP); }
  nap(0.3);
  CHECK(wd > 0 && running(wd), "signals: the watchdog died of SIGINT, SIGTERM or SIGHUP");
  double t0 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  ev_t w;
  CHECK(wait_for("write", t0, server, 1, 3, &w) == 1 && w.v == 0x0403, "signals: no clear after the client's death");
  show_err("signals");
  case_end(server, wd);
}

// R: the session lost with the server alive. The client's own rescue clears; the watchdog does not stand down for it
// (a rescue that has started has cleared nothing yet), and when the client has gone it looks for itself: one more
// connection, a read that finds bus mastering off, nothing written.
static void case_rescue(void) {
  case_env(NULL);
  setenv("WD_DROP_AFTER", "2", 1);   // the client's read and its write with MASTER, then the connection goes
  pid_t server = spawn_server(), client = spawn_client("rescue"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  int rc = reap(client, 12);
  CHECK(rc == 0, "rescue: the client exited %d", rc);
  CHECK(wd && gone(wd, 3), "rescue: the watchdog did not exit");
  CHECK(file_has(errp, "the emergency bus-master clear exited 0"), "rescue: the client's own rescue did not clear");
  CHECK(file_has(errp, "client exited without a clean teardown"), "rescue: the watchdog stood down for a rescue that had only started");
  CHECK(count("accept", t0, server, NULL) == 2, "rescue: %d connections after the loss, want the rescue's and the watchdog's",
        count("accept", t0, server, NULL));
  CHECK(count("write", t0, server, NULL) == 1, "rescue: %d writes after the loss, want the rescue's one - the watchdog found it off",
        count("write", t0, server, NULL));
  CHECK(count("start", 0, 0, NULL) == 1 && card_get() == 0x0403, "rescue: an app was started, or the card ends at %#x", card_get());
  show_err("rescue");
  case_end(server, wd);
}

// R slow: the same loss, and the server takes 3 s over the dead session. The rescue connects into the backlog, gets no
// answer in its 1 s and exits 4 with nothing written; the client goes on and exits. The watchdog once stood down at
// 'R' and the card stayed at 0x407 with nobody left (round 2 of the review). The stand-in naps after EVERY
// connection, the rescue's dead one too, so the watchdog's is served about 6 s after the loss.
static void case_rescue_slow(void) {
  case_env("3000");
  setenv("WD_DROP_AFTER", "2", 1);
  pid_t server = spawn_server(), client = spawn_client("rescue"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  int rc = reap(client, 12);
  CHECK(rc == 0, "rescue, slow teardown: the client exited %d", rc);
  CHECK(file_has(errp, "the emergency bus-master clear exited 4"), "rescue, slow teardown: the rescue was meant to time out (exit 4) here");
  ev_t w;
  int writes = wait_for("write", t0, server, 1, 12, &w);
  CHECK(writes == 1 && w.v == 0x0403, "rescue, slow teardown: %d write(s) after the rescue gave up, want the watchdog's 0x0403", writes);
  nap(0.2);
  CHECK(card_get() == 0x0403, "rescue, slow teardown: the card ends at %#x, want 0x0403", card_get());
  CHECK(count("start", 0, 0, NULL) == 1, "rescue, slow teardown: an app was started under a live server");
  CHECK(wd && gone(wd, 3), "rescue, slow teardown: the watchdog did not exit");
  show_err("rescue, slow teardown");
  case_end(server, wd);
}

// no stdio: fd 2 closed ("2"), or 0-2 closed ("012"), before the transport opens
static void case_noio(const char *closed) {
  char name[32];
  snprintf(name, sizeof(name), "fds %s closed", closed);
  case_env(NULL);
  setenv("WD_CLOSE", closed, 1);
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  int nwd = children(client, &wd);
  CHECK(nwd == 1, "%s: the client has %d children, want the one watchdog", name, nwd);
  nap(0.2);
  double t0 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  ev_t eof, w;
  wait_for("eof", t0, server, 1, 0.5, NULL);
  CHECK(first("eof", t0, server, &eof) && eof.conn == 1 && eof.t - t0 < 0.5,
        "%s: the server did not see the client's connection end within 0.5 s (the watchdog holds it as its stderr?)", name);
  int writes = wait_for("write", t0, server, 1, 3, &w);
  CHECK(writes == 1 && w.v == 0x0403 && w.t - t0 < 1.5, "%s: %d write(s) after the kill, want one of 0x0403 within about 1 s (%.2f s)",
        name, writes, writes ? w.t - t0 : -1.0);
  nap(0.2);
  CHECK(card_get() == 0x0403, "%s: the card ends at %#x, want 0x0403", name, card_get());
  CHECK(count("bad", 0, server, NULL) == 0 && count("other", 0, server, NULL) == 0,
        "%s: the server read %d thing(s) that are no request of this client's (somebody's log went into the session)", name,
        count("bad", 0, server, NULL) + count("other", 0, server, NULL));
  CHECK(wd && gone(wd, 3), "%s: the watchdog did not exit (it holds the pipe's write end itself?)", name);
  if (wd && !gone(wd, 0)) { kill(wd, SIGKILL); wd = 0; }
  case_end(server, wd);
}

// no stdio, the rescue: its spawn handed fd 2 on as well. Seen with the client alive, so only the rescue can have cleared.
static void case_noio_rescue(void) {
  case_env(NULL);
  setenv("WD_CLOSE", "2", 1);
  setenv("WD_DROP_AFTER", "2", 1);
  pid_t server = spawn_server(), client = spawn_client("rescue-stay"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  CHECK(wait_for("rescued", t0, client, 1, 11, NULL) == 1, "fd 2 closed, rescue: the client's rescue did not return");
  CHECK(kill(client, 0) == 0 && count("write", t0, server, NULL) == 1 && card_get() == 0x0403,
        "fd 2 closed, rescue: the rescue did not clear with the client alive (%d write(s), card %#x) - was it started at all?",
        count("write", t0, server, NULL), card_get());
  CHECK(count("bad", 0, server, NULL) == 0 && count("other", 0, server, NULL) == 0, "fd 2 closed, rescue: the server read something that is no request");
  kill(client, SIGKILL);
  reap(client, 3);
  CHECK(wd && gone(wd, 3), "fd 2 closed, rescue: the watchdog did not exit");
  if (wd && !gone(wd, 0)) { kill(wd, SIGKILL); wd = 0; }
  case_end(server, wd);
}

// slow app: the server killed under an idle armed client, and the app the watchdog starts listens only after 12 s -
// past the clear's 10 s, so that clear exits 124 with the app still coming up and no socket file (start_server
// unlinked it). killed: the client dies 3 s in; the watchdog's last clear got ENOENT, which it did not wait on, and
// exited 3 with nobody left (round 3 of the review). !killed: the client stays alive and idle, and the clear has to
// be tried again without waiting for anybody's death - and counted as done only when it is.
static void case_slow_app(int killed) {
  const char *name = killed ? "slow app, client killed" : "slow app, client idle";
  case_env(NULL);
  if (script_is_on) printf("  (slow app again, through the script)\n");
  pid_t server = spawn_server();
  setenv("WD_APP_DELAY_MS", "12000", 1);   // from here on: the stand-in app, not the server above
  pid_t client = spawn_client("idle"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(server, SIGKILL);
  waitpid(server, NULL, 0);
  if (killed) { nap(3); kill(client, SIGKILL); reap(client, 3); }
  ev_t w;
  int writes = wait_for("write", t0, 0, 1, 26, &w);
  CHECK(writes == 1 && w.v == 0x0403 && w.t - t0 > 11.5, "%s: %d write(s) after the server's death, want 0x0403 once the app listens (12 s)", name, writes);
  nap(0.3);
  CHECK(card_get() == 0x0403, "%s: the card ends at %#x, want 0x0403", name, card_get());
  CHECK(file_has(errp, "the clear exited 124"), "%s: the first clear was meant to time out behind the slow app", name);
  if (!killed) {
    CHECK(kill(client, 0) == 0 && wd && running(wd), "slow app, client idle: the client or its watchdog is gone");
    // and a clear that has landed is the last: nothing more while the client lives, nothing at its death
    double t1 = now();
    nap(6);
    kill(client, SIGKILL);
    reap(client, 3);
    CHECK(wd && gone(wd, 3), "slow app, client idle: the watchdog did not exit after the client's death");
    CHECK(count("accept", t1, 0, NULL) == 0, "slow app, client idle: %d connection(s) after the clear had landed", count("accept", t1, 0, NULL));
  } else {
    CHECK(wd && gone(wd, 3), "slow app, client killed: the watchdog did not exit");
    CHECK(file_has(errp, "has no socket yet"), "slow app, client killed: the last clear did not meet the missing socket file this case is about");
  }
  CHECK(count("start", t0, 0, NULL) == 1, "%s: the app was started %d times, want once", name, count("start", t0, 0, NULL));
  via_script(name, t0);
  show_err(name);
  case_end(0, wd);
}

// sticky: the client's quiesce writes MASTER off and the card keeps it. Its read-back says so, so it must not call
// itself clean, and the watchdog clears once it has gone.
static void case_sticky(void) {
  case_env(NULL);
  setenv("WD_STICKY", "1", 1);
  pid_t server = spawn_server(), client = spawn_client("clean"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  CHECK(reap(client, 3) == 0, "sticky: the client did not exit 0");
  ev_t w = {0};
  wait_for("write", t0, server, 2, 3, &w);
  CHECK(w.conn == 2 && w.v == 0x0403, "sticky: no clear from the watchdog after a quiesce that left MASTER set");
  nap(0.2);
  CHECK(card_get() == 0x0403, "sticky: the card ends at %#x, want 0x0403", card_get());
  CHECK(!file_has(errp, "stood down: clean"), "sticky: the watchdog was told the session was clean");
  show_err("sticky");
  case_end(server, wd);
}

// no fds: stdio closed and the table full, so what socket() and pipe() put on 0-2 cannot be moved.
//   "2", none free:   the socket is on fd 2 and stays. It was used there, and "no watchdog: pipe: ..." went into it.
//                     Now the open fails and nothing reaches the server.
//   "02" / "012", two free: the socket and the pipe's read end take the two; the write end stays on fd 2 / fd 1, where
//                     the program's own output is the watchdog's bytes ("Clean" ends in a session called clean).
//                     Now there is no watchdog at all, which the client can at least be told (TINYNV_WATCHDOG=0's line).
static void case_nofds(const char *closed, const char *fill) {
  char name[48];
  snprintf(name, sizeof(name), "no fds (%s closed, %s free)", closed, fill);
  case_env(NULL);
  setenv("WD_CLOSE", closed, 1);
  setenv("WD_FILL", fill, 1);
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  int nwd = children(client, &wd);
  if (!strcmp(fill, "none")) {
    CHECK(count("openfail", 0, client, NULL) == 1 && count("armed", 0, client, NULL) == 0, "%s: the open did not fail - a session on fd 2 is in use", name);
    CHECK(reap(client, 3) == 2, "%s: the client did not exit 2", name);
  } else {
    CHECK(count("armed", 0, client, NULL) == 1, "%s: the client did not arm", name);
    CHECK(nwd == 0, "%s: a watchdog is running with the pipe's write end on the client's fd 0-2", name);
    kill(client, SIGKILL);
    reap(client, 3);
  }
  nap(0.3);
  CHECK(count("bad", 0, server, NULL) == 0 && count("other", 0, server, NULL) == 0, "%s: the server read %d thing(s) that are no request", name,
        count("bad", 0, server, NULL) + count("other", 0, server, NULL));
  if (wd && !gone(wd, 3)) kill(wd, SIGKILL);
  case_end(server, 0);
}

// refork: the pipe is shared by every process in the session, so what one of them last said is not what another may
// assume. The parent armed; its child quiesced ('C') and went; the parent set MASTER again. A client that kept "I
// have said 'A'" to itself sent nothing, and the watchdog called the kill clean with the card at 0x407.
static void case_refork(void) {
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("refork"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  CHECK(wait_for("rearmed", t0, client, 1, 3, NULL) == 1 && card_get() == 0x0407, "refork: the parent did not turn bus mastering on again (card %#x)", card_get());
  double t1 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  ev_t w;
  int writes = wait_for("write", t1, server, 1, 3, &w);
  CHECK(writes == 1 && w.v == 0x0403 && w.t - t1 < 1.5, "refork: %d write(s) after the kill, want one of 0x0403 within about 1 s", writes);
  nap(0.2);
  CHECK(card_get() == 0x0403, "refork: the card ends at %#x, want 0x0403", card_get());
  CHECK(!file_has(errp, "stood down: clean"), "refork: the watchdog called an armed session clean");
  CHECK(wd && gone(wd, 3), "refork: the watchdog did not exit");
  show_err("refork");
  case_end(server, wd);
}

// 6: switched off
static void case_off(const char *var) {
  case_env(NULL);
  setenv(var, "0", 1);
  pid_t server = spawn_server(), client = spawn_client("idle");
  int nwd = children(client, NULL);
  CHECK(nwd == 0, "%s=0: the client has %d children", var, nwd);
  double t0 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  nap(1.5);
  CHECK(count("accept", t0, 0, NULL) == 0, "%s=0: something connected after the kill", var);
  setenv(var, "1", 1);
  case_end(server, 0);
}

// 8: a server slow to finish the dead session
static void case_slow(void) {
  case_env("3000");
  pid_t server = spawn_server(), client = spawn_client("idle"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGKILL);
  reap(client, 3);
  ev_t w;
  int writes = wait_for("write", t0, server, 1, 12, &w);
  CHECK(writes == 1 && w.v == 0x0403 && w.t - t0 > 2.8 && w.t - t0 < 10, "slow: %d write(s), want 0x0403 between 3 and 10 s after the kill (%.2f s)",
        writes, writes ? w.t - t0 : -1.0);
  if (fails) show_err("slow");
  case_end(server, wd);
}

// a watchdog that died first must not take the client with it through SIGPIPE
static void case_sigpipe(void) {
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("clean"), wd = 0;
  children(client, &wd);
  CHECK(wd > 0, "sigpipe: no watchdog to kill");
  if (wd > 0) { kill(wd, SIGKILL); nap(0.2); }
  kill(client, SIGUSR1);
  int rc = reap(client, 3);
  CHECK(rc == 0, "sigpipe: the clean teardown exited %d with the watchdog gone (141 is SIGPIPE)", rc);
  case_end(server, 0);
}

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "server")) return role_server(argv[2]);
  if (argc == 4 && !strcmp(argv[1], "client")) return role_client(argv[2], argv[3]);
  if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/') {
    fprintf(stderr, "usage: test_watchdog /absolute/path/to/tinynv-bmclear /absolute/path/to/tools/tinygpu-server.sh\n");
    return 2;
  }
  helper = argv[1];
  script = argv[2];
  if (!realpath(argv[0], self)) { perror("realpath"); return 2; }
  signal(SIGPIPE, SIG_IGN);   // the harness only; each client resets it to the default
  // Before anything is spawned: every way to the real app or a real socket points at this test's stand-ins.
  setenv("TINYGPU_APP", self, 1);
  setenv("TINYNV_BMCLEAR_HELPER", helper, 1);
  setenv("TINYNV_EMERGENCY_BM", "1", 1);
  setenv("TINYNV_WATCHDOG", "1", 1);
  unsetenv("TINYNV_SOCKET");
  snprintf(root, sizeof(root), "/tmp/tgwd-root.XXXXXX");   // EGPU_ROOT: where the script puts logs/, never the repo's
  if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
  setenv("EGPU_ROOT", root, 1);
  unsetenv("TINYGPU_START_NOWAIT");
  script_on(0);

  case_clean("clean");
  case_clean("unarmed");
  case_killed(0);
  case_killed(1);
  case_server_dies();
  case_together();
  script_on(1);
  case_script();
  case_server_dies();
  case_together();
  case_slow_app(1);
  script_on(0);
  case_unreadable();
  case_rescue();
  case_rescue_slow();
  case_noio("2");
  case_noio("012");
  case_noio_rescue();
  case_refork();
  case_sticky();
  case_nofds("2", "none");
  case_nofds("02", "two");
  case_nofds("012", "two");
  case_slow_app(1);
  case_slow_app(0);
  case_fork(0);
  case_fork(1);
  case_signals();
  case_exec();
  case_off("TINYNV_WATCHDOG");
  case_off("TINYNV_EMERGENCY_BM");
  case_slow();
  // an armed client out without its teardown: cleared, like a kill
  case_env(NULL);
  pid_t server = spawn_server(), client = spawn_client("exit"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  reap(client, 3);
  ev_t w;
  CHECK(wait_for("write", t0, server, 1, 3, &w) == 1 && w.v == 0x0403, "exit without teardown: bus mastering was not cleared");
  case_end(server, wd);
  case_sigpipe();

  char rm[128];
  snprintf(rm, sizeof(rm), "rm -rf '%s'", root);   // this test's own EGPU_ROOT, made by mkdtemp above
  if (system(rm)) {}
  printf("test_watchdog: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

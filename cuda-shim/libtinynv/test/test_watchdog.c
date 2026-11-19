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
//   R             the session lost with the server alive: the client's own rescue clears, the watchdog stands down
//   6 off        TINYNV_WATCHDOG=0, TINYNV_EMERGENCY_BM=0: no watchdog process, no clear after kill -9
//   8 slow        the server spends 3 s tearing the dead session down: the clear still lands, inside the hard timeout
//   exit          an armed client that exits without its teardown (TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL=1's path): cleared
//   sigpipe       the watchdog killed first, then a clean teardown with SIGPIPE at default: the client exits 0
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
  const char *td = getenv("WD_TEARDOWN_MS"), *drop = getenv("WD_DROP_AFTER");
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
    // WD_DROP_AFTER=N: the first connection is dropped after N requests and the server stays up - a lost session the
    // client finds by itself, so its own rescue (M3) runs and the watchdog has to stand down
    for (int nreq = 0; !(conn == 1 && drop && nreq == atoi(drop)) && recv(s, &rq, sizeof(rq), MSG_WAITALL) == (ssize_t)sizeof(rq); nreq++) {
      resp_t rp = {0};
      if (rq.cmd == CMD_CFG_READ && rq.arg0 == 4) { rp.resp0 = card_get(); logev("read %d %#llx", conn, (unsigned long long)rp.resp0); }
      else if (rq.cmd == CMD_CFG_WRITE && rq.arg0 == 4) { card_set((uint32_t)rq.arg2); logev("write %d %#llx", conn, (unsigned long long)rq.arg2); }
      else logev("other %d %u", conn, rq.cmd);
      if (send(s, &rp, sizeof(rp), 0) < 0) break;
    }
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
  tinynv_pci_t p;
  if (tinynv_pci_open_tinygpu(sock, &p)) { fprintf(stderr, "client: open: %s\n", tinynv_last_error()); return 2; }
  uint32_t cmd = p.cfg_read(&p, 4, 2);
  if (strcmp(mode, "unarmed")) p.cfg_write(&p, 4, 2, cmd | 4);
  logev("armed %s", mode);
  int sig;
  sigwait(&us, &sig);
  if (!strcmp(mode, "clean")) { p.quiesce(&p); logev("quiesced"); return 0; }
  if (!strcmp(mode, "rescue")) {
    p.cfg_read(&p, 4, 2);   // the server is gone: this latches the loss
    p.rescue(&p, 9.0);
    return 0;
  }
  return 0;   // "exit", "unarmed": out without a teardown
}

// ---- the harness ----
static const char *helper;
static char self[PATH_MAX], dir[64], sock[128], logp[160], card[160], errp[160];

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
  setenv("WD_LOG", logp, 1);
  setenv("WD_CARD", card, 1);
  if (teardown_ms) setenv("WD_TEARDOWN_MS", teardown_ms, 1); else unsetenv("WD_TEARDOWN_MS");
  unsetenv("WD_DROP_AFTER");
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
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 2, errp, O_WRONLY | O_CREAT | O_APPEND, 0600);
  pid_t pid;
  if (posix_spawn(&pid, self, &fa, NULL, argv, environ)) { perror("spawn client"); exit(2); }
  posix_spawn_file_actions_destroy(&fa);
  if (wait_for("armed", 0, pid, 1, 3, NULL) < 1) printf("note: client %d (%s) did not arm\n", (int)pid, mode);
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
static int file_has(const char *path, const char *needle) {
  FILE *f = fopen(path, "r");
  char line[512];
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
  for (int i = 0; i < n; i++) if (!strcmp(e[i].ev, "start") && e[i].pid != server) kill(e[i].pid, SIGKILL);
  if (server > 0) { kill(server, SIGKILL); waitpid(server, NULL, 0); }
  char lock[200];
  snprintf(lock, sizeof(lock), "%s.bmclear.lock", sock);
  unlink(lock); unlink(sock); unlink(logp); unlink(card); unlink(errp);
  rmdir(dir);
}
static void show_err(const char *what) {
  static int shown;
  if (fails == shown) return;   // only for a case that has just failed
  shown = fails;
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
  if (fails) show_err("server -9");
  case_end(0, wd);
}

// 5: the client's own rescue and the watchdog's server-death trigger, together
static void case_together(void) {
  case_env(NULL);
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
  CHECK(card_get() == 0x0403, "two helpers at once: the card ends at %#x", card_get());
  case_end(0, 0);
}

// 'R' on its own: the session lost with the server alive, the client's own rescue clears, the watchdog stands down
static void case_stand_down(void) {
  case_env(NULL);
  setenv("WD_DROP_AFTER", "2", 1);   // the client's read and its write with MASTER, then the connection goes
  pid_t server = spawn_server(), client = spawn_client("rescue"), wd = 0;
  children(client, &wd);
  double t0 = now();
  kill(client, SIGUSR1);
  int rc = reap(client, 12);
  CHECK(rc == 0, "stand down: the client exited %d", rc);
  CHECK(wd && gone(wd, 3), "stand down: the watchdog did not exit");
  char want[96];
  snprintf(want, sizeof(want), "stood down: client pid %d started its own emergency clear", (int)client);
  CHECK(file_has(errp, want), "stand down: the watchdog did not say \"%s\"", want);
  CHECK(count("accept", t0, server, NULL) == 1, "stand down: %d connections after the loss, want the rescue's one",
        count("accept", t0, server, NULL));
  CHECK(count("start", 0, 0, NULL) == 1 && card_get() == 0x0403, "stand down: an app was started, or the card ends at %#x", card_get());
  CHECK(!file_has(errp, "client exited without a clean teardown"), "stand down: the watchdog cleared as well");
  show_err("stand down");
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
  if (argc != 2 || argv[1][0] != '/') { fprintf(stderr, "usage: test_watchdog /absolute/path/to/tinynv-bmclear\n"); return 2; }
  helper = argv[1];
  if (!realpath(argv[0], self)) { perror("realpath"); return 2; }
  signal(SIGPIPE, SIG_IGN);   // the harness only; each client resets it to the default
  // Before anything is spawned: every way to the real app or a real socket points at this test's stand-ins.
  setenv("TINYGPU_APP", self, 1);
  setenv("TINYNV_BMCLEAR_HELPER", helper, 1);
  setenv("TINYNV_EMERGENCY_BM", "1", 1);
  setenv("TINYNV_WATCHDOG", "1", 1);
  unsetenv("TINYNV_SOCKET");

  case_clean("clean");
  case_clean("unarmed");
  case_killed(0);
  case_killed(1);
  case_server_dies();
  case_together();
  case_stand_down();
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

  printf("test_watchdog: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

// tinynv-bmclear (M3 of the 2026-11-18 DART panic analysis) against a server this test plays, with no card and no
// TinyGPU: the helper is run as its own process, exactly as libtinynv starts it, and every path it could take to
// the real thing is pointed elsewhere - --sock at a socket in a fresh /tmp directory, --app and $TINYGPU_APP at a
// stand-in script, --old-pid at a pid this test chose. Nothing here can reach $TMPDIR/tinygpu.sock or the installed app.
//
//   bus master on    read PCI_COMMAND 0x0407, write 0x0403, read back 0x0403: exactly three requests, exit 0
//   all ones         0xffff, and the transport's 0xffffffff: one read and nothing written, exit 6
//   no answer        the server takes the request and never replies (another client, or wedged): nothing written,
//                    exit 4 after the reply timeout; and with a longer reply timeout the hard --timeout stops it
//   start            nobody listening, the old server gone: the app is started as `APP server PATH`
//   no start         nobody listening, the old server alive - or another process serving the same path, however
//                    it is spelt: it is NOT replaced, and nothing is started
//   usage            no --sock: refused, since the socket is never guessed
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
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
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_CFG_READ = 3, CMD_CFG_WRITE = 4 };

static const char *helper;
static char dir[64], sock[128], app[160], marker[160];
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

static pid_t run(const char *const *extra) {
  const char *argv[16] = {helper, "--sock", sock, "--app", app};
  int n = 5;
  for (; extra && *extra && n < 15; extra++) argv[n++] = *extra;
  argv[n] = NULL;
  pid_t pid;
  if (posix_spawn(&pid, helper, NULL, NULL, (char **)argv, environ)) { perror("spawn"); exit(2); }
  return pid;
}
static int reap(pid_t pid, double limit) {
  int st;
  double t0 = now();
  while (waitpid(pid, &st, WNOHANG) == 0) {
    if (now() - t0 > limit) { kill(pid, SIGKILL); waitpid(pid, &st, 0); return -1; }   // the helper this test started
    usleep(10000);
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static int listener(void) {
  int l = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, sock, sizeof(a.sun_path) - 1);
  unlink(sock);
  if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1)) { perror("listen"); exit(2); }
  return l;
}
// one request, within a second, or 0
static int take(int fd, req_t *rq) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  if (poll(&p, 1, 1000) != 1) return 0;
  return recv(fd, rq, sizeof(*rq), MSG_WAITALL) == (ssize_t)sizeof(*rq);
}
static void reply(int fd, uint64_t v) { resp_t rp = {.status = 0, .resp0 = v}; if (send(fd, &rp, sizeof(rp), 0) < 0) perror("reply"); }
static int at_eof(int fd) { req_t rq; struct pollfd p = {.fd = fd, .events = POLLIN}; return poll(&p, 1, 2000) == 1 && recv(fd, &rq, sizeof(rq), 0) == 0; }

// a server answering PCI_COMMAND reads with `cmd` (and later with what was written), logging every request
static void serve_cmd(uint32_t cmd, int want_write) {
  int l = listener();
  pid_t pid = run(NULL);
  int s = accept(l, NULL, NULL);
  req_t rq[3];
  int ok = take(s, &rq[0]) && rq[0].cmd == CMD_CFG_READ && rq[0].arg0 == 4 && rq[0].arg1 == 2;
  CHECK(ok, "PCI_COMMAND %#x: the first request is not a 2-byte read of PCI_COMMAND (cmd %u off %llu)", cmd, rq[0].cmd, (unsigned long long)rq[0].arg0);
  if (ok) reply(s, cmd);
  if (want_write) {
    ok = take(s, &rq[1]) && rq[1].cmd == CMD_CFG_WRITE && rq[1].arg0 == 4 && rq[1].arg1 == 2 && rq[1].arg2 == (cmd & ~4u);
    CHECK(ok, "PCI_COMMAND %#x: the second request is not the write of %#x (cmd %u val %#llx)", cmd, cmd & ~4u, rq[1].cmd, (unsigned long long)rq[1].arg2);
    if (ok) reply(s, 0);
    ok = take(s, &rq[2]) && rq[2].cmd == CMD_CFG_READ && rq[2].arg0 == 4;
    CHECK(ok, "PCI_COMMAND %#x: the third request is not the read-back", cmd);
    if (ok) reply(s, cmd & ~4u);
  }
  CHECK(at_eof(s), "PCI_COMMAND %#x: the helper sent more than %d request(s)", cmd, want_write ? 3 : 1);
  int rc = reap(pid, 6.0);
  CHECK(rc == (want_write ? 0 : 6), "PCI_COMMAND %#x: the helper exited %d, want %d", cmd, rc, want_write ? 0 : 6);
  close(s); close(l); unlink(sock);
}

int main(int argc, char **argv) {
  if (argc != 2 || argv[1][0] != '/') { fprintf(stderr, "usage: test_bmclear /absolute/path/to/tinynv-bmclear\n"); return 2; }
  helper = argv[1];
  signal(SIGPIPE, SIG_IGN);
  snprintf(dir, sizeof(dir), "/tmp/tgbm.XXXXXX");
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
  snprintf(sock, sizeof(sock), "%s/tinygpu.sock", dir);
  snprintf(app, sizeof(app), "%s/TinyGPU-stand-in.sh", dir);
  snprintf(marker, sizeof(marker), "%s/started", dir);
  FILE *f = fopen(app, "w");
  fprintf(f, "#!/bin/sh\necho \"$@\" >> '%s'\n", marker);   // a server that records being started and serves nothing
  fclose(f);
  chmod(app, 0700);
  setenv("TINYGPU_APP", app, 1);   // the same stand-in, should anything fall back to the environment
  char slog[160];
  snprintf(slog, sizeof(slog), "%s/server.log", dir);
  setenv("TINYGPU_SERVER_LOG", slog, 1);   // never the repository's logs/tinygpu-server.log
  // and the direct spawn, never tools/tinygpu-server.sh, whose snapshots go to the repository's logs/ (test_watchdog
  // runs the script's path, with EGPU_ROOT a temp directory)
  setenv("TINYGPU_SERVER_SCRIPT", "/nonexistent/tools/tinygpu-server.sh", 1);

  serve_cmd(0x0407, 1);
  serve_cmd(0xffff, 0);
  serve_cmd(0xffffffff, 0);

  // no answer: the request is taken and never replied to
  int l = listener();
  double t0 = now();
  pid_t pid = run((const char *[]){"--reply-timeout", "0.5", NULL});
  int s = accept(l, NULL, NULL);
  req_t rq;
  CHECK(take(s, &rq) && rq.cmd == CMD_CFG_READ, "no answer: the first request is not a read");
  int rc = reap(pid, 6.0);
  double took = now() - t0;
  CHECK(rc == 4, "no answer: the helper exited %d, want 4", rc);
  CHECK(took < 2.0, "no answer: the helper took %.2f s to give up", took);
  CHECK(at_eof(s), "no answer: the helper sent something after the unanswered read");
  close(s); close(l);
  // ...and the hard timeout, below the reply timeout
  l = listener();
  t0 = now();
  pid = run((const char *[]){"--reply-timeout", "4", "--timeout", "1", NULL});
  s = accept(l, NULL, NULL);
  CHECK(take(s, &rq) && rq.cmd == CMD_CFG_READ, "hard timeout: the first request is not a read");
  rc = reap(pid, 6.0);
  took = now() - t0;
  CHECK(rc == 124 && took < 2.5, "hard timeout: the helper exited %d after %.2f s, want 124 after about 1 s", rc, took);
  CHECK(at_eof(s), "hard timeout: the helper sent something after the unanswered read");
  close(s); close(l); unlink(sock);

  // start: nothing listening, the old server gone (a child that has exited)
  pid_t gone = fork();
  if (!gone) _exit(0);
  waitpid(gone, NULL, 0);
  char gp[16]; snprintf(gp, sizeof(gp), "%d", (int)gone);
  pid = run((const char *[]){"--old-pid", gp, "--timeout", "1", NULL});
  rc = reap(pid, 6.0);
  char line[256] = "", want[256];
  f = fopen(marker, "r");
  if (f) { if (!fgets(line, sizeof(line), f)) line[0] = 0; fclose(f); }
  snprintf(want, sizeof(want), "server %s\n", sock);
  CHECK(!strcmp(line, want), "start: the stand-in app was started with \"%s\", want \"%s\"", line, want);
  CHECK(rc == 124, "start: a server that never listens should end in the hard timeout, exit %d", rc);
  unlink(marker);

  // no start: nothing listening, and the old server ALIVE (this test's own pid): never replaced
  char me[16]; snprintf(me, sizeof(me), "%d", (int)getpid());
  pid = run((const char *[]){"--old-pid", me, NULL});
  rc = reap(pid, 6.0);
  CHECK(rc == 3, "no start: exit %d, want 3", rc);
  CHECK(access(marker, F_OK) != 0, "no start: a server was started while the old one is alive");

  // no start: nothing listening, the old server gone, but ANOTHER process serves this path, spelt differently
  // ("dir//tinygpu.sock"): a server busy with its one client refuses connects, and must not be replaced
  char other[160]; snprintf(other, sizeof(other), "%s//tinygpu.sock", dir);
  const char *sh[] = {"/bin/sh", "-c", "sleep 3; :", "server", other, NULL};
  pid_t busy;
  if (posix_spawn(&busy, "/bin/sh", NULL, NULL, (char **)sh, environ)) { perror("spawn"); return 2; }
  usleep(100000);
  pid = run((const char *[]){"--old-pid", gp, NULL});
  rc = reap(pid, 6.0);
  CHECK(rc == 3, "another server for the path: exit %d, want 3", rc);
  CHECK(access(marker, F_OK) != 0, "another server for the path: a second server was started");
  kill(busy, SIGKILL); waitpid(busy, NULL, 0);   // the stand-in this test started
  unlink(marker);

  // usage: the socket is never guessed
  const char *bare[] = {helper, "--old-pid", "1", NULL};
  if (posix_spawn(&pid, helper, NULL, NULL, (char **)bare, environ)) { perror("spawn"); return 2; }
  CHECK(reap(pid, 3.0) == 2, "no --sock: the helper did not refuse");

  char lock[200];
  snprintf(lock, sizeof(lock), "%s.bmclear.lock", sock);   // the helper's own, never removed by it (tools/tinynv_bmclear.c)
  unlink(app); unlink(marker); unlink(sock); unlink(lock); unlink(slog); rmdir(dir);
  printf("test_bmclear: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

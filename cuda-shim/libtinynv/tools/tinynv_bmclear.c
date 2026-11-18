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
//    that script starts it: `APP server PATH`, in a session of its own so it outlives this. --app, else $TINYGPU_APP,
//    else the installed app. A live server that refuses connections is busy or wedged and is never replaced.
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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <libgen.h>
#include <limits.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_CFG_READ = 3, CMD_CFG_WRITE = 4 };
#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x4
#define DEFAULT_APP "/Applications/TinyGPU.app/Contents/MacOS/TinyGPU"

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
  write(2, m, sizeof(m) - 1);
  _exit(124);
}

static int pid_alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

// One spelling per socket: the directory resolved (symlinks, "//", /var vs /private/var), the name kept - the socket
// itself may be gone. So a server started as "server $TMPDIR/tinygpu.sock" is found whichever way the client spelt it.
static void norm(const char *path, char *out, size_t n) {
  char d[PATH_MAX], b[PATH_MAX], r[PATH_MAX];
  snprintf(d, sizeof(d), "%s", path);
  snprintf(b, sizeof(b), "%s", path);
  if (realpath(dirname(d), r)) snprintf(out, n, "%s/%s", r, basename(b));
  else snprintf(out, n, "%s", path);
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

static int start_server(const char *app, const char *sock) {
  struct stat st;
  if (!lstat(sock, &st) && !S_ISSOCK(st.st_mode)) { say("%s exists and is not a socket; not starting a server on it", sock); return -1; }
  unlink(sock);   // a stale socket file: nothing accepts on it (checked above) and no process serves it
  char *argv[] = {(char *)app, "server", (char *)sock, NULL};
  posix_spawnattr_t at;
  posix_spawn_file_actions_t fa;
  posix_spawnattr_init(&at);
  short flags = 0;
#ifdef POSIX_SPAWN_SETSID
  flags |= POSIX_SPAWN_SETSID;
#endif
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
  posix_spawnattr_setflags(&at, flags);
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, 2, 1);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  posix_spawn_file_actions_addinherit_np(&fa, 2);
#endif
  pid_t pid;
  int rc = posix_spawn(&pid, app, &fa, &at, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc) { say("could not start %s server %s: %s", app, sock, strerror(rc)); return -1; }
  say("started %s server %s (pid %d)", app, sock, (int)pid);
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

int main(int argc, char **argv) {
  const char *sock = NULL, *app = getenv("TINYGPU_APP");
  int old_pid = 0, no_start = 0;
  double timeout = 5.0, reply_timeout = 1.0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--no-start")) { no_start = 1; continue; }
    if (!v) { fprintf(stderr, "tinynv-bmclear: %s needs a value\n", a); return 2; }
    if (!strcmp(a, "--sock")) sock = v;
    else if (!strcmp(a, "--old-pid")) old_pid = atoi(v);
    else if (!strcmp(a, "--app")) app = v;
    else if (!strcmp(a, "--timeout")) timeout = atof(v);
    else if (!strcmp(a, "--reply-timeout")) reply_timeout = atof(v);
    else { fprintf(stderr, "tinynv-bmclear: unknown argument %s\n", a); return 2; }
    i++;
  }
  // No default socket: which server to reach is the caller's to say, never guessed from this process's environment.
  if (!sock || !*sock || timeout <= 0 || reply_timeout <= 0) {
    fprintf(stderr, "usage: tinynv-bmclear --sock PATH [--old-pid N] [--app PATH] [--timeout S] [--reply-timeout S] [--no-start]\n");
    return 2;
  }
  if (!app || !*app) app = DEFAULT_APP;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGALRM, on_alarm);
  alarm((unsigned)(timeout + 0.999));
  say("clearing bus mastering through %s (old server pid %d %s)", sock, old_pid,
      old_pid <= 0 ? "unknown" : pid_alive(old_pid) ? "alive" : "gone");

  int fd = connect_to(sock, reply_timeout);
  if (fd < 0) {
    int e = errno, other = server_process_for(sock);
    if (pid_alive(old_pid) || other) {
      say("a server for %s may be alive (pid %d) but refuses connections (%s): busy with another client or wedged; not touching it",
          sock, pid_alive(old_pid) ? old_pid : other, strerror(e));
      return 3;
    }
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

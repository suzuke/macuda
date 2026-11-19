// TinyGPU backend: on macOS a signed helper owns the DriverKit user client, so config space, BAR windows and DMA memory are
// reached over a unix socket. Wire format and semantics: docs/driver/architecture.md §2 (identical to the python client in
// tinygrad/runtime/support/system.py, so the two can be diffed). Writes are posted and carry no reply; reads are round trips.
#include "internal.h"
#include "exe_path.h"
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;
// tools/tinynv_bmclear.c, built beside this library; the Makefile bakes in where. TINYNV_BMCLEAR_HELPER overrides it,
// and a release's copy beside the executable comes before the baked path (tinynv_pci_bmclear_helper).
#ifndef TINYNV_BMCLEAR_DEFAULT
#define TINYNV_BMCLEAR_DEFAULT "tinynv-bmclear"
#endif

// Resolved ONCE, before any session exists (tinynv_pci_open_tinygpu calls this first), and only read afterwards:
// tg_rescue can run inside the crash handler, where realpath and stat have no business. The environment override is
// not part of it - it is read where the helper is started, as it always was, and when it is set at resolve time
// nothing is said here. The first executable file of: <exe dir>/tinynv-bmclear, <prefix>/libexec/macuda/tinynv-bmclear,
// the baked TINYNV_BMCLEAR_DEFAULT. Said once when that is not the baked one (a developer build, whose test binaries
// sit beside it, stays quiet); said loudly when there is none, because then a lost server or a killed client leaves
// bus mastering on and nothing will clear it.
static char bm_found[PATH_MAX];
static pthread_once_t bm_once = PTHREAD_ONCE_INIT;
static void bm_resolve(void) {
  const char *env = getenv("TINYNV_BMCLEAR_HELPER");
  int quiet = env && *env;
  char dir[PATH_MAX], pre[PATH_MAX], cand[3][PATH_MAX + 48];
  int nc = 0;
  if (!tinynv_exe_dir(dir, sizeof(dir))) {
    snprintf(cand[nc++], sizeof(cand[0]), "%s/tinynv-bmclear", dir);
    if (!tinynv_exe_prefix(dir, pre, sizeof(pre))) snprintf(cand[nc++], sizeof(cand[0]), "%s/libexec/macuda/tinynv-bmclear", pre);
  }
  snprintf(cand[nc++], sizeof(cand[0]), "%s", TINYNV_BMCLEAR_DEFAULT);
  for (int i = 0; i < nc; i++) {
    if (!tinynv_is_exec(cand[i])) continue;
    int baked = i == nc - 1 || tinynv_same_file(cand[i], TINYNV_BMCLEAR_DEFAULT);
    snprintf(bm_found, sizeof(bm_found), "%s", baked ? TINYNV_BMCLEAR_DEFAULT : cand[i]);
    if (!baked && !quiet)
      fprintf(stderr, "libtinynv: tinynv-bmclear from %s (relative to the executable; TINYNV_BMCLEAR_HELPER overrides it)\n", bm_found);
    return;
  }
  snprintf(bm_found, sizeof(bm_found), "%s", TINYNV_BMCLEAR_DEFAULT);   // so a start fails with this name, as it did
  if (quiet) return;
  char looked[3 * (PATH_MAX + 48) + 8] = "";
  for (int i = 0; i < nc; i++) { strcat(looked, i ? ", " : ""); strcat(looked, cand[i]); }
  fprintf(stderr, "libtinynv: no tinynv-bmclear found (looked in %s): no emergency bus-master clear and no watchdog\n", looked);
}
const char *tinynv_pci_bmclear_helper(void) {
  pthread_once(&bm_once, bm_resolve);
  return bm_found;
}

enum {
  CMD_PROBE = 0, CMD_MAP_BAR, CMD_MAP_SYSMEM_FD, CMD_CFG_READ, CMD_CFG_WRITE, CMD_RESET,
  CMD_MMIO_READ, CMD_MMIO_WRITE, CMD_MAP_SYSMEM, CMD_SYSMEM_READ, CMD_SYSMEM_WRITE, CMD_RESIZE_BAR, CMD_PING,
};

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
_Static_assert(sizeof(req_t) == 33, "request is 33 packed bytes");
_Static_assert(sizeof(resp_t) == 17, "response is 17 packed bytes");

#define PCI_COMMAND 0x04
#define PCI_COMMAND_MASTER 0x4
#define MAX_DMA 128 // the server keeps at most this many mappings per session and frees none of them

typedef struct {
  int fd;
  // THE SERVER IS GONE, latched by the first transfer that hit end of file or a socket error and never cleared. On
  // 2026-11-18 the server's session ended mid-upload and every later read came back 0xffffffff, which submit.c then
  // reported as the device reading all ones; the investigation chased the card. From here on every call fails at once
  // without touching the socket, and transport_dead lets a caller name the real cause.
  int dead;
  char dead_why[160];
  int split_err_said; // a split read's error reply is printed the first time only; tinynv_last_error keeps the latest
  // A read whose request has been sent and whose reply has not been taken. Any other request must first take that
  // reply (the socket carries one conversation), so rpc() drains it into pending_val and rd32_recv finds it there.
  int pending, pending_done;
  uint32_t pending_val;
  int ndma;
  struct { void *va; size_t size; } dma[MAX_DMA];
  uint64_t bar_size[TINYNV_MAX_BARS];
  // For the emergency bus-master clear (tg_rescue): the socket this session was opened on and the server process at
  // its other end, so the helper reaches the same server path and can tell whether the old server is still alive.
  char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  int peer_pid;
  int rescue_state; // 0 not started, 1 running, 2 finished, refused or turned off
  pid_t rescue_pid;
  // The watchdog's clean channel (M4, tg_watch): the write end of its pipe, -1 when there is none. Nothing is kept
  // here about what was last written: the pipe is shared by every process a fork puts in this session, so the last
  // byte in it may be another process's (tg_wd_say).
  int wd_fd;
} tg_t;


// TINYNV_RPC_STATS=1: what crosses the socket, counted per command and kind, printed at exit. A read is a round trip
// (the caller waits for the server); a write is posted (sent, never answered); an async read is sent now and collected
// later. MMIO offsets are tallied too, so a steady-state count can be pinned to the registers behind it. Off by
// default and then a handful of branches per message. Counters are relaxed atomics: exact under the driver's lock, close outside it.
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
enum { RK_TRIP, RK_POSTED, RK_ASYNC, RK_N };
static int g_rs = -1;
static _Atomic unsigned long g_rn[16][RK_N], g_rbytes[16][RK_N], g_rwait_ns;
#define RS_SLOTS 4096
static struct { _Atomic unsigned long key, n; } g_roff[2][RS_SLOTS];   // [0] reads, [1] writes; key = bar<<40|offset, +1
static _Atomic unsigned long g_roff_lost[2];   // offsets that found the table full (since the boot mark, once it is set)
static unsigned long g_rboot[16][RK_N], g_rboot_wait; static int g_rbooted;   // the counters when the boot finished
static const char *rs_cmd(int c) {
  static const char *n[] = {"probe", "map_bar", "map_sysmem_fd", "cfg_read", "cfg_write", "reset", "mmio_read", "mmio_write",
                            "map_sysmem", "sysmem_read", "sysmem_write", "resize_bar", "ping"};
  return c >= 0 && c < 13 ? n[c] : "?";
}
static void rs_dump(void) {
  static const char *kn[] = {"round trips", "posted", "async reads"};
  fprintf(stderr, "libtinynv: rpc stats (TINYNV_RPC_STATS=1): per command and kind, count and payload bytes\n");
  unsigned long trips = 0;
  for (int c = 0; c < 16; c++)
    for (int k = 0; k < RK_N; k++) {
      unsigned long n = atomic_load(&g_rn[c][k]);
      if (!n) continue;
      if (k != RK_POSTED) trips += n;
      fprintf(stderr, "libtinynv:   rpc %-14s %-12s %10lu  %12lu bytes\n", rs_cmd(c), kn[k], n, atomic_load(&g_rbytes[c][k]));
    }
  fprintf(stderr, "libtinynv:   rpc total: %lu answered (round trips + async reads), %.1f ms blocked waiting on round trips\n",
          trips, atomic_load(&g_rwait_ns) / 1e6);
  if (g_rbooted) {
    unsigned long at = 0;
    for (int c = 0; c < 16; c++)
      for (int k = 0; k < RK_N; k++) {
        unsigned long n = atomic_load(&g_rn[c][k]) - g_rboot[c][k];
        if (!n) continue;
        if (k != RK_POSTED) at += n;
        fprintf(stderr, "libtinynv:   rpc after boot: %-14s %-12s %10lu\n", rs_cmd(c), kn[k], n);
      }
    fprintf(stderr, "libtinynv:   rpc after boot total: %lu answered, %.1f ms blocked waiting; offsets below are after boot only\n",
            at, (atomic_load(&g_rwait_ns) - g_rboot_wait) / 1e6);
  }
  for (int w = 0; w < 2; w++) {
    fprintf(stderr, "libtinynv:   rpc busiest mmio %s offsets (bar:offset count)%s\n", w ? "write" : "read",
            atomic_load(&g_roff_lost[w]) ? " - table full, some offsets not shown" : "");
    for (int shown = 0; shown < 10; shown++) {
      int best = -1; unsigned long bn = 0;
      for (int i = 0; i < RS_SLOTS; i++) { unsigned long n = atomic_load(&g_roff[w][i].n); if (n > bn) { bn = n; best = i; } }
      if (best < 0) break;
      unsigned long key = atomic_load(&g_roff[w][best].key) - 1;
      fprintf(stderr, "libtinynv:     %lu:%#08lx %10lu\n", key >> 40, key & ((1ul << 40) - 1), bn);
      atomic_store(&g_roff[w][best].n, 0);
    }
  }
}
#include <pthread.h>
static void rs_init(void) { const char *v = getenv("TINYNV_RPC_STATS"); g_rs = v && *v == '1'; if (g_rs) atexit(rs_dump); }
static int rs_on(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  if (g_rs < 0) pthread_once(&once, rs_init);   // once: two threads racing here would register the report twice
  return g_rs;
}
static void rs_count(int cmd, int kind, uint64_t bytes) {
  if (!rs_on()) return;
  atomic_fetch_add_explicit(&g_rn[cmd & 15][kind], 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_rbytes[cmd & 15][kind], bytes, memory_order_relaxed);
}
static void rs_off(int write, uint32_t bar, uint64_t off) {
  if (!rs_on()) return;
  unsigned long key = ((unsigned long)bar << 40 | (off & ((1ul << 40) - 1))) + 1;
  for (unsigned i = 0, h = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 52); i < RS_SLOTS; i++, h = (h + 1) % RS_SLOTS) {
    unsigned long k = atomic_load(&g_roff[write][h].key);
    if (!k) { unsigned long z = 0; if (!atomic_compare_exchange_strong(&g_roff[write][h].key, &z, key) && z != key) continue; k = key; }
    if (k == key) { atomic_fetch_add_explicit(&g_roff[write][h].n, 1, memory_order_relaxed); return; }
  }
  atomic_fetch_add_explicit(&g_roff_lost[write], 1, memory_order_relaxed);
}
void tinynv_rpc_stats_boot_done(void) {
  if (!rs_on() || g_rbooted) return;
  for (int c = 0; c < 16; c++) for (int k = 0; k < RK_N; k++) g_rboot[c][k] = atomic_load(&g_rn[c][k]);
  g_rboot_wait = atomic_load(&g_rwait_ns);
  // The table starts again EMPTY, keys and all: the boot touches thousands of registers, and keeping their keys left
  // no room for the offsets the steady state uses (the after-boot report then showed leftovers of the boot).
  for (int w = 0; w < 2; w++) {
    for (int i = 0; i < RS_SLOTS; i++) { atomic_store(&g_roff[w][i].n, 0); atomic_store(&g_roff[w][i].key, 0); }
    atomic_store(&g_roff_lost[w], 0);
  }
  g_rbooted = 1;
}
static uint64_t rs_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// Latch the loss and say so ONCE, then push the line to disk: the 2026-11-18 panic came 32 s after the loss and the
// client's log (a regular file, tools/serve.sh) lost its last ~30 s, so an unsynced line may as well not be written.
// Classification only: what each caller gets back is the same sentinel it got before.
static int tg_lose(tg_t *tg, const char *what) {
  if (!tg->dead) {
    tg->dead = 1;
    snprintf(tg->dead_why, sizeof(tg->dead_why), "%s", what);
    fprintf(stderr, "libtinynv: tinygpu server connection lost (%s). Every later call to the device fails at once "
            "without touching the socket, and reads from here on are this transport's 0xffffffff, not the device's\n", what);
    fflush(stderr);
    struct stat st;
    if (fstat(2, &st) == 0 && S_ISREG(st.st_mode)) {
#ifdef F_FULLFSYNC
      if (fcntl(2, F_FULLFSYNC) == -1)
#endif
        fsync(2);
    }
  }
  return tinynv_fail("tinygpu server connection lost (%s)", tg->dead_why);
}

#ifdef MSG_NOSIGNAL
#define TG_SEND_FLAGS MSG_NOSIGNAL
#else
#define TG_SEND_FLAGS 0
#endif
static int xfer_all(tg_t *tg, void *buf, size_t len, int sending) {
  if (tg->dead) return tinynv_fail("tinygpu server connection lost (%s)", tg->dead_why);
  for (size_t off = 0; off < len;) {
    ssize_t n = sending ? send(tg->fd, (char *)buf + off, len - off, TG_SEND_FLAGS) : recv(tg->fd, (char *)buf + off, len - off, 0);
    if (n > 0) { off += (size_t)n; continue; }
    // An interrupted call fails this transfer as it always did and latches nothing: a signal is not a lost server,
    // and a latch here would turn the signal handler's bus-master clear (tg_quiesce) into a no-op.
    if (n < 0 && errno == EINTR) return tinynv_fail("tinygpu: %s interrupted after %zu of %zu bytes", sending ? "send" : "recv", off, len);
    char what[96];
    snprintf(what, sizeof(what), "%s %s after %zu of %zu bytes", sending ? "send" : "recv",
             n == 0 ? "hit end of file" : strerror(errno), off, len);
    return tg_lose(tg, what);
  }
  return 0;
}

// Take the reply of a split read that is still on the socket: the header, an error message if it carries one, and
// the four bytes read. Kept for rd32_recv, which may be asked for it after other requests have gone by.
static uint32_t tg_take_pending(tg_t *tg) {
  resp_t rp;
  uint32_t v = 0xffffffffu;
  tg->pending = 0;
  if (xfer_all(tg, &rp, sizeof(rp), 0)) return v;
  if (rp.status != 0) { // the server's reason, kept rather than read and dropped
    char msg[256] = "unknown error";
    size_t n = rp.resp0 < sizeof(msg) - 1 ? (size_t)rp.resp0 : sizeof(msg) - 1;
    if (n && !xfer_all(tg, msg, n, 0)) msg[n] = 0;
    if (!tg->dead) {
      tinynv_fail("tinygpu: split mmio read failed: %s", msg);
      if (!tg->split_err_said++) fprintf(stderr, "libtinynv: tinygpu: a split mmio read failed at the server: %s (said once)\n", msg);
    }
    return v;
  }
  xfer_all(tg, &v, 4, 0);
  return v;
}
// one request, one reply. payload is appended to the request; readout is the bulk data that follows the reply.
static int rpc(tg_t *tg, resp_t *rp, uint8_t cmd, uint32_t bar, uint64_t a0, uint64_t a1, uint64_t a2,
               const void *payload, size_t payload_len, void *readout, size_t readout_len, int *fd_out) {
  if (tg->pending) { tg->pending_val = tg_take_pending(tg); tg->pending_done = 1; }
  req_t rq = {.cmd = cmd, .dev_id = 0, .bar = bar, .arg0 = a0, .arg1 = a1, .arg2 = a2};
  rs_count(cmd, RK_TRIP, payload_len + readout_len);
  if (cmd == CMD_MMIO_READ) rs_off(0, bar, a0);
  uint64_t t0 = g_rs > 0 ? rs_now() : 0;
  if (xfer_all(tg, &rq, sizeof(rq), 1)) return -1;
  if (payload_len && xfer_all(tg, (void *)payload, payload_len, 1)) return -1;

  if (fd_out) { // the reply carries a descriptor for the shared memory the server just created
    *fd_out = -1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = rp, .iov_len = sizeof(*rp)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    ssize_t n = recvmsg(tg->fd, &msg, 0);
    if (n == 0 || (n < 0 && errno != EINTR)) return tg_lose(tg, n ? strerror(errno) : "recvmsg hit end of file");
    if (n != (ssize_t)sizeof(*rp)) return tinynv_fail("tinygpu: short reply (%zd bytes)", n);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd_out, CMSG_DATA(c), sizeof(int));
  } else if (xfer_all(tg, rp, sizeof(*rp), 0)) return -1;

  if (rp->status != 0) { // the message follows the reply, resp0 bytes of it
    char msg[256] = "unknown error";
    size_t n = rp->resp0 < sizeof(msg) - 1 ? (size_t)rp->resp0 : sizeof(msg) - 1;
    if (n && !xfer_all(tg, msg, n, 0)) msg[n] = 0;
    if (tg->dead) return -1; // the message itself was cut off by the loss, which tg_lose has already recorded
    return tinynv_fail("tinygpu: command %u failed: %s", cmd, msg);
  }
  if (readout_len && xfer_all(tg, readout, readout_len, 0)) return -1;
  if (t0) atomic_fetch_add_explicit(&g_rwait_ns, rs_now() - t0, memory_order_relaxed);
  return 0;
}

// One byte to the watchdog: 'A' bus mastering may be on, 'C' it was read back off. The watchdog goes by the LAST
// byte it received, whoever sent it, so each one is sent EVERY time and nothing is remembered here: this process's
// "I have said 'A'" is no longer true once a child it forked has quiesced and said 'C' into the same pipe - the
// parent then set MASTER again, sent nothing, was killed, and the watchdog called it clean with the card at 0x407
// (found by review, 2026-11-19). The cost is one 1-byte write per PCI_COMMAND write with MASTER (dev.c: two a boot)
// and per DMA mapping (at most MAX_DMA a session), next to a socket round trip each.
// The pipe is non-blocking and raises no SIGPIPE (tg_watch), so a watchdog that died first costs nothing here; a
// full pipe would lose the byte, and the watchdog reads every byte as it arrives.
static void tg_wd_say(tg_t *tg, char what) {
  if (tg->wd_fd >= 0 && write(tg->wd_fd, &what, 1) < 0) {}
}

// A descriptor of this session's - the server socket, an end of the watchdog's pipe - is never 0, 1 or 2. A program
// that closed its stdio (a daemon, `2>&-`) gets those numbers from socket() and pipe(), and then fd 2 IS the session:
// libtinynv's own fprintf(stderr) went to the server as requests, and both spawns below, which hand fd 2 on as the
// child's stdout and stderr, gave the watchdog the client's server socket (its log text became requests and the
// session outlived the client) or the pipe's own write end (it never saw end of file). Either way a killed client
// left the card at 0x407 with nobody to clear it (found by review, 2026-11-19). Close-on-exec comes with the move.
// NO HALF MEASURE when it cannot be moved (EMFILE: the table is full): the descriptor is CLOSED and -1 comes back.
// Kept where it was, a socket on fd 2 took the library's stderr as requests and a pipe end on fd 1 took the program's
// stdout as the watchdog's bytes - a 'C' in it, and a killed session was called clean (round 3 of the review). So
// the caller does without: no session at all (tinynv_pci_open_tinygpu fails), or no watchdog (tg_watch).
static int tg_fd_high(int fd) {
  if (fd < 0 || fd > 2) return fd;
  int hi = fcntl(fd, F_DUPFD_CLOEXEC, 3), e = errno;
  close(fd);
  errno = e;
  return hi;
}

// What a spawned helper gets as stdio: /dev/null to read, and this process's stderr as its stdout and stderr, so its
// lines land in this process's log - when this process has one. fd 2 is never part of the session (tg_fd_high), so
// an open fd 2 is the program's own; a closed one is /dev/null for the child, because a dup2 from a closed fd 2 fails
// the whole spawn and there would be no watchdog and no rescue at all for a program without a stderr. fcntl only:
// tg_rescue can run inside the crash handler.
static void tg_child_stdio(posix_spawn_file_actions_t *fa) {
  posix_spawn_file_actions_addopen(fa, 0, "/dev/null", O_RDONLY, 0);
  if (fcntl(2, F_GETFD) != -1) {
    posix_spawn_file_actions_adddup2(fa, 2, 1);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    posix_spawn_file_actions_addinherit_np(fa, 2);
#endif
  } else {
    posix_spawn_file_actions_addopen(fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(fa, 2, "/dev/null", O_WRONLY, 0);
  }
}

static uint32_t tg_cfg_read(tinynv_pci_t *p, uint32_t off, uint32_t size) {
  resp_t rp;
  if (rpc(p->ctx, &rp, CMD_CFG_READ, 0, off, size, 0, NULL, 0, NULL, 0, NULL)) return 0xffffffff;
  return (uint32_t)rp.resp0;
}

static void tg_cfg_write(tinynv_pci_t *p, uint32_t off, uint32_t size, uint32_t val) {
  resp_t rp;
  // armed BEFORE the write goes out: a client killed between the two must not leave a watchdog that believes it clean
  if (off == PCI_COMMAND && (val & PCI_COMMAND_MASTER)) tg_wd_say(p->ctx, 'A');
  rpc(p->ctx, &rp, CMD_CFG_WRITE, 0, off, size, val, NULL, 0, NULL, 0, NULL);
}

static int tg_bar_info(tinynv_pci_t *p, int bar, uint64_t *base, uint64_t *size) {
  tg_t *tg = p->ctx;
  resp_t rp;
  if (bar < 0 || bar >= TINYNV_MAX_BARS) return tinynv_fail("tinygpu: bar %d out of range", bar);
  if (rpc(tg, &rp, CMD_MAP_BAR, (uint32_t)bar, 0, 0, 0, NULL, 0, NULL, 0, NULL)) return -1;
  tg->bar_size[bar] = rp.resp1;
  if (base) *base = rp.resp0; // the server's own mapping address, not a bus address
  if (size) *size = rp.resp1;
  return 0;
}

// the window never lives in this address space: each access is one message, and writes are posted
static uint32_t tg_rd32(tinynv_mmio_t *m, uint64_t off) {
  uint32_t v = 0xffffffff;
  resp_t rp;
  rpc(m->ctx, &rp, CMD_MMIO_READ, (uint32_t)m->bar, m->off + off, 4, 0, NULL, 0, &v, 4, NULL);
  return v;
}
// The split read over the socket: the request goes now and the reply stays on the socket until asked for. Posted
// writes may go between; another request first drains it (rpc above).
static int tg_rd32_send(tinynv_mmio_t *m, uint64_t off) {
  tg_t *tg = m->ctx;
  if (tg->pending) { tg->pending_val = tg_take_pending(tg); tg->pending_done = 1; }
  tg->pending_done = 0;
  req_t rq = {.cmd = CMD_MMIO_READ, .bar = (uint32_t)m->bar, .arg0 = m->off + off, .arg1 = 4};
  rs_count(CMD_MMIO_READ, RK_ASYNC, 4); rs_off(0, (uint32_t)m->bar, m->off + off);
  if (xfer_all(tg, &rq, sizeof(rq), 1)) return -1;
  tg->pending = 1;
  return 0;
}
static uint32_t tg_rd32_recv(tinynv_mmio_t *m) {
  tg_t *tg = m->ctx;
  if (tg->pending_done) { tg->pending_done = 0; return tg->pending_val; }
  if (tg->pending) return tg_take_pending(tg);
  return 0xffffffffu;
}
static void tg_wr32(tinynv_mmio_t *m, uint64_t off, uint32_t v) {
  tg_t *tg = m->ctx;
  req_t rq = {.cmd = CMD_MMIO_WRITE, .bar = (uint32_t)m->bar, .arg0 = m->off + off, .arg1 = 4};
  rs_count(CMD_MMIO_WRITE, RK_POSTED, 4); rs_off(1, (uint32_t)m->bar, m->off + off);
  if (!xfer_all(tg, &rq, sizeof(rq), 1)) xfer_all(tg, &v, 4, 1);
}
static void tg_rd_block(tinynv_mmio_t *m, uint64_t off, void *dst, size_t n) {
  resp_t rp;
  if (rpc(m->ctx, &rp, CMD_MMIO_READ, (uint32_t)m->bar, m->off + off, n, 0, NULL, 0, dst, n, NULL)) memset(dst, 0xff, n);
}
static void tg_wr_block(tinynv_mmio_t *m, uint64_t off, const void *src, size_t n) {
  tg_t *tg = m->ctx;
  req_t rq = {.cmd = CMD_MMIO_WRITE, .bar = (uint32_t)m->bar, .arg0 = m->off + off, .arg1 = n};
  rs_count(CMD_MMIO_WRITE, RK_POSTED, n); rs_off(1, (uint32_t)m->bar, m->off + off);
  if (!xfer_all(tg, &rq, sizeof(rq), 1)) xfer_all(tg, (void *)src, n, 1);
}

static int tg_bar_map(tinynv_pci_t *p, int bar, uint64_t off, uint64_t size, tinynv_mmio_t *out) {
  tg_t *tg = p->ctx;
  uint64_t bar_size;
  if (tg_bar_info(p, bar, NULL, &bar_size)) return -1;
  if (!size) size = bar_size - off;
  if (off + size > bar_size) return tinynv_fail("tinygpu: window [%#llx,%#llx) is outside bar %d of %#llx bytes",
                                         (unsigned long long)off, (unsigned long long)(off + size), bar, (unsigned long long)bar_size);
  *out = (tinynv_mmio_t){.ptr = NULL, .ctx = tg, .bar = bar, .off = off, .size = size,
                         .rd32 = tg_rd32, .wr32 = tg_wr32, .rd_block = tg_rd_block, .wr_block = tg_wr_block,
                         .rd32_send = tg_rd32_send, .rd32_recv = tg_rd32_recv};
  return 0;
}

static void tg_bar_unmap(tinynv_pci_t *p, tinynv_mmio_t *m) { (void)p; memset(m, 0, sizeof(*m)); }

static int tg_dma_alloc(tinynv_pci_t *p, size_t size, tinynv_dma_t *out) {
  tg_t *tg = p->ctx;
  resp_t rp;
  int fd = -1;
  if (tg->ndma >= MAX_DMA) return tinynv_fail("tinygpu: the server allows %d dma mappings per session", MAX_DMA);
  tg_wd_say(tg, 'A');   // a mapping the server will complete when the session ends, with whatever mastering the card has
  if (rpc(tg, &rp, CMD_MAP_SYSMEM_FD, 0, size, 0, 0, NULL, 0, NULL, 0, &fd)) return -1;
  if (fd < 0) return tinynv_fail("tinygpu: dma reply carried no descriptor");

  size_t mapped = rp.resp0;
  void *va = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (va == MAP_FAILED) return tinynv_fail("tinygpu: mmap of %zu dma bytes failed: %s", mapped, strerror(errno));

  // the server leaves the mapping described at its own head: (address, length) pairs, terminated by (0, 0)
  size_t npages = mapped / 0x1000;
  uint64_t *pages = calloc(npages, sizeof(uint64_t)), *seg = va;
  size_t n = 0;
  for (size_t i = 0; seg[2 * i + 1] && n < npages; i++)
    for (uint64_t o = 0; o < seg[2 * i + 1] && n < npages; o += 0x1000) pages[n++] = seg[2 * i] + o;
  if (n < npages) {
    free(pages);
    munmap(va, mapped);
    return tinynv_fail("tinygpu: dma mapping describes %zu of %zu pages (the dext truncates past 32 segments)", n, npages);
  }

  tg->dma[tg->ndma].va = va;
  tg->dma[tg->ndma].size = mapped;
  tg->ndma++;
  // WHERE THE LARGE HOST MAPPINGS SIT ON THE BUS, every session. A DART fault is latched as an IOVA and nothing else, and
  // until this line no log could say which buffer an IOVA belonged to in the session that faulted: on 2026-10-14 two
  // latches at 0x84ac8000 (21:23, 23:32) could be placed only by a September boot trace, which put the GSP-RM image
  // there. One line per mapping of 1 MiB or more - a handful per boot - with the first and last page and the segments.
  if (mapped >= (1u << 20)) {
    size_t nseg = 0; while (seg[2 * nseg + 1]) nseg++;
    fprintf(stderr, "tinynv: dma mapping %d: %zu bytes, iova %#llx..%#llx in %zu segment(s)\n", tg->ndma - 1, mapped,
            (unsigned long long)pages[0], (unsigned long long)(pages[npages - 1] + 0xfff), nseg);
  }
  *out = (tinynv_dma_t){.va = va, .size = mapped, .pages = pages, .npages = npages, .ctx = tg};
  tinynv_dma_bind_host(out);
  return 0;
}

static void tg_dma_free(tinynv_pci_t *p, tinynv_dma_t *dma) {
  (void)p; // the server owns the pages until the connection drops: only the page list is ours to release
  free(dma->pages);
  dma->pages = NULL;
  dma->npages = 0;
}

static int tg_reset(tinynv_pci_t *p) {
  resp_t rp;
  return rpc(p->ctx, &rp, CMD_RESET, 0, 0, 0, 0, NULL, 0, NULL, 0, NULL);
}

// With the loss latched, the read below fails at once and reads 0xffffffff, so this returns without a write - which
// is what it did before the latch existed, when the same read went to a dead socket and came back the same way.
static void tg_quiesce(tinynv_pci_t *p) {
  uint32_t cmd = tg_cfg_read(p, PCI_COMMAND, 2);
  // gone: a failed server call reads 0xffffffff (tg_cfg_read), an absent function's 2-byte read 0xffff. Writing
  // cmd & ~MASTER back for the second would put 0xfffb in the command register of whatever answers there next.
  if (cmd == 0xffffffff || (cmd & 0xffff) == 0xffff) return;
  tg_cfg_write(p, PCI_COMMAND, 2, cmd & ~PCI_COMMAND_MASTER);
  uint32_t after = tg_cfg_read(p, PCI_COMMAND, 2); // read back, so the write has landed before any mapping is torn down
  // Clean only on what the card said: a read-back that failed, or still has MASTER, leaves the watchdog armed.
  if (after != 0xffffffff && (after & 0xffff) != 0xffff && !(after & PCI_COMMAND_MASTER)) tg_wd_say(p->ctx, 'C');
}

static int tg_transport_dead(tinynv_pci_t *p) { tg_t *tg = p->ctx; return tg && tg->dead; }

// M3 of the 2026-11-18 DART panic analysis. With the server gone this process has no door to the card at all, and the
// dext has completed every mapping with bus mastering still on, so a running GSP-RM faults on each DMA until the
// kernel gives up (32 s and a panic that night). A separate process - tinynv-bmclear - opens a fresh connection, to
// the same server path (starting a server first only if none exists for it) and clears PCI_COMMAND's bus master bit.
// A separate process because a fresh IOServiceOpen can block behind the dead session in the dext's serial queue (H3):
// the helper bounds itself and this process bounds its wait for the helper.
//
// Gates, in order: only for a loss THIS process latched (tg->dead); once; TINYNV_EMERGENCY_BM=0 turns it off. The
// helper holds the rest: it never starts a second server while one for the path exists, and it writes nothing until
// the server has answered its first read, which proves no other client holds the one-client server.
//
// posix_spawn only - this can run inside the crash handler. The child gets no descriptor but stdio (no lock file, no
// dead socket), with stderr shared so its result lands in this process's log (tg_child_stdio: /dev/null when this
// process has no stderr of its own), and an empty signal mask (the handler runs with its own signal blocked).
//
// The watchdog is told nothing of this. It once stood down on a byte sent here ('R'), when the rescue had only
// STARTED: with the server 3 s in the dead session's teardown the rescue's 1 s reply timeout expired (exit 4), the
// watchdog had already gone, and the card stayed at 0x407 (found by review, 2026-11-19). It now acts on what it sees
// - this process gone, or the server gone - and the helper's lock puts the two in a row: whichever comes second
// connects to the server that is up, reads PCI_COMMAND, writes only if MASTER is still set, and starts no server.
static void tg_rescue(tinynv_pci_t *p, double wait_s) {
  tg_t *tg = p->ctx;
  if (!tg || !tg->dead) return;
  if (!tg->rescue_state) {
    tg->rescue_state = 2;
    const char *off = getenv("TINYNV_EMERGENCY_BM");
    if (off && !strcmp(off, "0")) {
      fprintf(stderr, "libtinynv: TINYNV_EMERGENCY_BM=0: no emergency bus-master clear; the card may go on mastering "
              "into mappings the server has let go\n");
      return;
    }
    const char *helper = getenv("TINYNV_BMCLEAR_HELPER");
    if (!helper || !*helper) helper = bm_found;   // resolved at open (bm_resolve): no lookup here, this may be the crash handler
    char pid[16];
    snprintf(pid, sizeof(pid), "%d", tg->peer_pid);
    char *argv[] = {(char *)helper, "--sock", tg->path, "--old-pid", pid, NULL};
    posix_spawnattr_t at;
    posix_spawn_file_actions_t fa;
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_init(&at);
    posix_spawnattr_setsigmask(&at, &none);
    short flags = POSIX_SPAWN_SETSIGMASK;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&at, flags);
    posix_spawn_file_actions_init(&fa);
    tg_child_stdio(&fa);
    int rc = posix_spawn(&tg->rescue_pid, helper, &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc) {
      fprintf(stderr, "libtinynv: could not start the emergency bus-master clear (%s): %s\n", helper, strerror(rc));
      return;
    }
    tg->rescue_state = 1;
    fprintf(stderr, "libtinynv: emergency bus-master clear started: %s --sock %s --old-pid %s (pid %d)\n", helper,
            tg->path, pid, (int)tg->rescue_pid);
  }
  if (tg->rescue_state != 1 || wait_s <= 0) return;
  // Bounded. The helper stops itself (5 s by default); this is the bound in case it cannot.
  struct timespec t0, t, nap = {0, 10 * 1000 * 1000};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int st = 0;
  pid_t r;
  for (;;) {
    r = waitpid(tg->rescue_pid, &st, WNOHANG);
    if (r != 0 && !(r < 0 && errno == EINTR)) break;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) * 1e-9 > wait_s) break;
    nanosleep(&nap, NULL);
  }
  if (r == 0) {
    kill(tg->rescue_pid, SIGKILL);   // our own child, the helper - never the server
    waitpid(tg->rescue_pid, &st, 0);
    fprintf(stderr, "libtinynv: the emergency bus-master clear did not finish in %.0f s and was stopped\n", wait_s);
  } else if (r > 0 && WIFEXITED(st)) {
    fprintf(stderr, "libtinynv: the emergency bus-master clear exited %d%s\n", WEXITSTATUS(st),
            WEXITSTATUS(st) ? " - bus mastering was NOT confirmed off, its lines above say why" : ": bus mastering is off");
  } else if (r > 0) {
    fprintf(stderr, "libtinynv: the emergency bus-master clear died of signal %d\n", WIFSIGNALED(st) ? WTERMSIG(st) : 0);
  }
  tg->rescue_state = 2;
}

static void tg_close(tinynv_pci_t *p) {
  tg_t *tg = p->ctx;
  if (!tg) return;
  for (int i = 0; i < tg->ndma; i++) munmap(tg->dma[i].va, tg->dma[i].size);
  if (tg->fd >= 0) close(tg->fd);
  if (tg->wd_fd >= 0) close(tg->wd_fd);   // after the socket: an armed session closed is the watchdog's to clear
  free(tg);
  p->ctx = NULL;
}

// M4 of the 2026-11-18 DART panic analysis: a watchdog beside every session, tinynv-bmclear --watch, for the two ways
// out that M2 and M3 cannot see because they need this process alive and talking - this process killed or crashed
// under a live server, which then tears the session down with bus mastering on, and the server dying while this
// process sits idle and touches nothing. The helper's header says what it does and when; what matters here:
//
//   - The clean channel is a pipe. This process holds the write end, non-blocking and with SIGPIPE off: a watchdog
//     that died first must not kill the client, which was the step-B bug with the server's socket. The watchdog holds
//     the read end as its fd 3, and end of file there is its only sign that the client has gone.
//   - THE PIPE'S WRITE END AND THE SERVER SOCKET GO TOGETHER: both close-on-exec, both kept across a fork. So every
//     process that holds the session holds the pipe, and end of file means exactly "nobody holds the session any
//     more". A fork whose parent exits leaves the child in the session, and the watchdog must not fire then: it
//     did when it watched the opener's pid, sat in the listen(1) backlog against the live session, and the next
//     program's connect was refused (found by review, 2026-11-19).
//   - The watchdog gets nothing else but stdio (POSIX_SPAWN_CLOEXEC_DEFAULT). Holding this process's server socket
//     would keep the session open after this process died, and the server would never see it go. Its stdio is never
//     the socket or the pipe either, whatever numbers this process's descriptors have (tg_fd_high, tg_child_stdio).
//     If the pipe's ends cannot be moved off 0-2 there is NO watchdog, and one line says so.
//   - NOT ATOMIC, and left so: macOS has no SOCK_CLOEXEC and no pipe2, so between socket()/pipe() and the fcntl that
//     sets close-on-exec another thread can fork and exec, and that child holds one of the two without the other -
//     the socket without the pipe (the watchdog fires against a session still open), or the pipe without the socket
//     (it never fires). Theoretical: it needs a fork+exec in another thread inside those few instructions, and a
//     posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT is not affected at all.
//   - The socket path goes in the environment, not argv: `pkill -f tinygpu.sock` stops servers, not watchdogs.
//   - A session of its own, and the helper ignores SIGINT/SIGHUP/SIGTERM: a ^C or a kill of this process group is
//     what it is there for.
//   - TINYNV_WATCHDOG=0 turns it off, and so does TINYNV_EMERGENCY_BM=0, which already meant "no bus-master clear
//     from anyone but this process's own teardown".
// macOS only: it watches pids with kqueue, and only the macOS server can be killed under a client this way.
static void tg_watch(tg_t *tg) {
#ifdef __APPLE__
  const char *off = getenv("TINYNV_WATCHDOG"), *em = getenv("TINYNV_EMERGENCY_BM");
  if ((off && !strcmp(off, "0")) || (em && !strcmp(em, "0"))) {
    fprintf(stderr, "libtinynv: %s=0: no watchdog; a kill of this process or of the server leaves bus mastering as it is\n",
            off && !strcmp(off, "0") ? "TINYNV_WATCHDOG" : "TINYNV_EMERGENCY_BM");
    return;
  }
  const char *helper = getenv("TINYNV_BMCLEAR_HELPER");
  if (!helper || !*helper) helper = bm_found;   // resolved by tinynv_pci_open_tinygpu before this
  int pfd[2];
  // no pipe2 here: see NOT ATOMIC above for the window before the two fcntls below
  if (pipe(pfd)) { fprintf(stderr, "libtinynv: no watchdog: pipe: %s\n", strerror(errno)); return; }
  // the read end above the three it becomes fd 3 among: a dup2 onto itself would keep its close-on-exec
  int rd = fcntl(pfd[0], F_DUPFD_CLOEXEC, 10), e = errno;
  close(pfd[0]);
  pfd[1] = tg_fd_high(pfd[1]);   // off 0-2, where pipe() puts it in a program without stdio; closed if it cannot be
  if (pfd[1] < 0) e = errno;
  // Both ends where they belong, or no pipe and no watchdog: an end left on 0-2 would be read or written as the
  // program's stdio. Said only now, with both ends closed and the socket long off 0-2 (tinynv_pci_open_tinygpu), so
  // this line can land in neither.
  if (rd < 0 || pfd[1] < 0) {
    if (rd >= 0) close(rd);
    if (pfd[1] >= 0) close(pfd[1]);
    fprintf(stderr, "libtinynv: no watchdog: no descriptor above 2 for its pipe (%s); a kill of this process or of the "
            "server leaves bus mastering as it is\n", strerror(e));
    return;
  }
  fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
  fcntl(pfd[1], F_SETFL, fcntl(pfd[1], F_GETFL) | O_NONBLOCK);
  fcntl(pfd[1], F_SETNOSIGPIPE, 1);

  size_t n = 0;
  while (environ[n]) n++;
  char **env = calloc(n + 2, sizeof(char *)), sockvar[sizeof(tg->path) + 32];
  size_t k = 0;
  for (size_t i = 0; i < n; i++) if (strncmp(environ[i], "TINYNV_BMCLEAR_SOCK=", 20)) env[k++] = environ[i];
  snprintf(sockvar, sizeof(sockvar), "TINYNV_BMCLEAR_SOCK=%s", tg->path);
  env[k++] = sockvar;
  char pid[16];
  snprintf(pid, sizeof(pid), "%d", tg->peer_pid);
  char *argv[] = {(char *)helper, "--watch", "--old-pid", pid, NULL};
  posix_spawnattr_t at;
  posix_spawn_file_actions_t fa;
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_init(&at);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
  posix_spawn_file_actions_init(&fa);
  tg_child_stdio(&fa);
  posix_spawn_file_actions_adddup2(&fa, rd, 3);
  pid_t wd;
  int rc = posix_spawn(&wd, helper, &fa, &at, argv, env);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  free(env);
  close(rd);
  if (rc) {
    close(pfd[1]);
    fprintf(stderr, "libtinynv: could not start the watchdog (%s): %s\n", helper, strerror(rc));
    return;
  }
  tg->wd_fd = pfd[1];
#else
  (void)tg;
#endif
}

int tinynv_pci_open_tinygpu(const char *sock_path, tinynv_pci_t *out) {
  tinynv_pci_bmclear_helper();   // first, outside any handler: tg_rescue and tg_watch only read what it stored
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(sock_path) >= sizeof(addr.sun_path)) return tinynv_fail("tinygpu: socket path is too long");
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  // No SOCK_CLOEXEC on macOS: close-on-exec is set below, and a fork+exec in another thread before that keeps the
  // socket (tg_watch: NOT ATOMIC). Off 0-2 first, where socket() puts it in a program that closed its stdio.
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return tinynv_fail("tinygpu: socket: %s", strerror(errno));
  // A session on fd 0-2 is never used: if the socket cannot be moved (the table is full) it is closed, before the
  // connect, and this open fails.
  int low = fd;
  fd = tg_fd_high(fd);
  if (fd < 0) return tinynv_fail("tinygpu: socket() gave fd %d and no descriptor above 2 is free to move it to (%s); a session "
                                 "on fd 0-2 would take this process's stdio as requests, so none is opened", low, strerror(errno));
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    close(fd);
    return tinynv_fail("tinygpu: no server on %s: %s", sock_path, strerror(errno));
  }
  // A killed server must cost this process a failed send (EPIPE, latched as a lost server), never a SIGPIPE: most
  // CUDA programs leave SIGPIPE at its default, and on 2026-11-19 (step B) llama-simple died of it the instant the
  // server went, before the loss could be latched or bus mastering cleared. Per socket, so the program's own
  // disposition is left alone; MSG_NOSIGNAL in xfer_all does the same where SO_NOSIGPIPE does not exist.
#ifdef SO_NOSIGPIPE
  { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
  fcntl(fd, F_SETFD, FD_CLOEXEC);   // as the watchdog's pipe (tg_watch): an exec leaves both behind, a fork keeps both
  int buf = 64 << 20;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

  tg_t *tg = calloc(1, sizeof(tg_t));
  tg->fd = fd;
  tg->wd_fd = -1;
  snprintf(tg->path, sizeof(tg->path), "%s", sock_path);
#ifdef LOCAL_PEERPID
  { pid_t peer = 0; socklen_t len = sizeof(peer); if (!getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer, &len)) tg->peer_pid = peer; }
#endif
  tg_watch(tg);
  *out = (tinynv_pci_t){.ctx = tg, .live = 1, .name = "usb4", .cfg_read = tg_cfg_read, .cfg_write = tg_cfg_write, .bar_info = tg_bar_info,
                        .bar_map = tg_bar_map, .bar_unmap = tg_bar_unmap, .dma_alloc = tg_dma_alloc, .dma_free = tg_dma_free,
                        .reset = tg_reset, .quiesce = tg_quiesce, .close = tg_close, .transport_dead = tg_transport_dead,
                        .rescue = tg_rescue};
  return 0;
}

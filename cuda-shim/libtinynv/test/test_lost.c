// A lost gpu, offline (M2 and the gpu half of M7 of the 2026-11-18 DART panic analysis): the REAL TinyGPU backend,
// submit.c, exec.c's waits and gpu.c's close, against a server this test plays on a unix socket of its own (a thread
// that answers every request and logs it, so what reached the "card" is SEEN, not inferred). No card, no TinyGPU.
//
//   short waits     a wait that ends inside 250 ms - already done, or done 100 ms in - and an ordinary announcement put
//                   no extra request on the socket; a 600 ms wait on a healthy card costs one BOOT_0 read per 250 ms and
//                   not one config read
//   window lost     the server answers but every BAR read is all ones and config space is fine: a stuck wait returns
//                   within half a second (not at its timeout), the loss is classified as the window and latched, and
//                   from then nothing is staged or rung - not one request more; the close then clears bus mastering
//                   (read, write without MASTER, read back), sends GSP-RM nothing, and only then is the socket closed
//   server gone     the server stops mid-wait: the wait returns within half a second, the loss is the server's, nothing
//                   more is sent, and the emergency bus-master clear is started once with this session's socket path
//                   and server pid - a stand-in script here, so nothing can reach the real server or app
#include "exec.h"
#include "internal.h"
#include "nv_regs.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_MAP_BAR = 1, CMD_CFG_READ = 3, CMD_CFG_WRITE = 4, CMD_MMIO_READ = 6, CMD_MMIO_WRITE = 7, EV_EOF = 100 };
enum { M_NORMAL, M_WINDOW, M_GONE };

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

// ---- the server: one thread, one client, every request logged ----
typedef struct { uint8_t cmd; uint32_t bar; uint64_t off, len, val; } ev_t;
static struct {
  int lfd, srv, mode;
  char dir[64], path[128];
  pthread_t th;
  pthread_mutex_t mu;
  ev_t log[4096];
  int n;
  uint32_t cfg_cmd;
  uint64_t mem_off[256]; uint32_t mem_val[256]; int nmem;   // BAR1 dwords written, so a read-back returns them
} S = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void note(uint8_t cmd, uint32_t bar, uint64_t off, uint64_t len, uint64_t val) {
  pthread_mutex_lock(&S.mu);
  if (S.n < 4096) S.log[S.n++] = (ev_t){cmd, bar, off, len, val};
  pthread_mutex_unlock(&S.mu);
}
static int logged(void) { pthread_mutex_lock(&S.mu); int n = S.n; pthread_mutex_unlock(&S.mu); return n; }
static int count_since(int from, int cmd) { int c = 0; pthread_mutex_lock(&S.mu); for (int i = from; i < S.n; i++) c += S.log[i].cmd == cmd; pthread_mutex_unlock(&S.mu); return c; }
static int count_boot0_since(int from) { int c = 0; pthread_mutex_lock(&S.mu); for (int i = from; i < S.n; i++) c += S.log[i].cmd == CMD_MMIO_READ && S.log[i].bar == 0 && S.log[i].off == NV_PMC_BOOT_0; pthread_mutex_unlock(&S.mu); return c; }
static void answer(uint64_t r0, uint64_t r1, const void *tail, size_t n) {
  if (S.mode == M_GONE) return;   // its sending side is shut: a gone server says nothing
  resp_t rp = {.status = 0, .resp0 = r0, .resp1 = r1};
  if (send(S.srv, &rp, sizeof(rp), 0) < 0 || (n && send(S.srv, tail, n, 0) < 0)) return;
}
static void *serve(void *arg) {
  (void)arg;
  for (;;) {
    req_t rq;
    if (recv(S.srv, &rq, sizeof(rq), MSG_WAITALL) != (ssize_t)sizeof(rq)) { note(EV_EOF, 0, 0, 0, 0); return NULL; }
    if (rq.cmd == CMD_MMIO_WRITE) {
      uint8_t pay[4096];
      size_t n = rq.arg1 < sizeof(pay) ? (size_t)rq.arg1 : sizeof(pay);
      if (recv(S.srv, pay, n, MSG_WAITALL) != (ssize_t)n) { note(EV_EOF, 0, 0, 0, 0); return NULL; }
      uint32_t v = 0; memcpy(&v, pay, n < 4 ? n : 4);
      note(rq.cmd, rq.bar, rq.arg0, rq.arg1, v);
      if (rq.bar == 1 && S.nmem < 256) { S.mem_off[S.nmem] = rq.arg0; S.mem_val[S.nmem++] = v; }
      continue;
    }
    note(rq.cmd, rq.bar, rq.arg0, rq.arg1, rq.arg2);
    if (rq.cmd == CMD_MAP_BAR) answer(0, 16 << 20, NULL, 0);
    else if (rq.cmd == CMD_CFG_READ) answer(rq.arg0 == 0 ? 0x10de : rq.arg0 == 4 ? S.cfg_cmd : 0, 0, NULL, 0);
    else if (rq.cmd == CMD_CFG_WRITE) { if (rq.arg0 == 4) S.cfg_cmd = (uint32_t)rq.arg2; answer(0, 0, NULL, 0); }
    else if (rq.cmd == CMD_MMIO_READ) {
      uint8_t out[64];
      size_t n = rq.arg1 < sizeof(out) ? (size_t)rq.arg1 : sizeof(out);
      memset(out, S.mode == M_WINDOW ? 0xff : 0, n);
      if (S.mode == M_NORMAL && rq.bar == 0 && rq.arg0 == NV_PMC_BOOT_0) { uint32_t id = 0xb77000a1; memcpy(out, &id, 4); }
      if (S.mode == M_NORMAL && rq.bar == 1)
        for (int i = S.nmem - 1; i >= 0; i--) if (S.mem_off[i] == rq.arg0) { memcpy(out, &S.mem_val[i], 4); break; }
      answer(n, 0, out, n);
    } else answer(0, 0, NULL, 0);
  }
}

// ---- the driver side ----
static tinynv_pci_t pci;
static tinynv_gpu_t g;
static tinynv_exec_t ex;
static tinynv_queue_t q;
static tinynv_paddr_range_t fifo_range = {.paddr = 0x100000, .size = 0x10000};
static uint8_t sem_host[4096] __attribute__((aligned(128)));
static void sem_set(uint64_t off, uint64_t v) { __atomic_store_n((uint64_t *)(sem_host + off), v, __ATOMIC_SEQ_CST); }

static void session_open(void) {
  snprintf(S.dir, sizeof(S.dir), "/tmp/tglost.XXXXXX");
  if (!mkdtemp(S.dir)) { perror("mkdtemp"); exit(2); }
  snprintf(S.path, sizeof(S.path), "%s/tinygpu.sock", S.dir);
  S.lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, S.path, sizeof(a.sun_path) - 1);
  if (S.lfd < 0 || bind(S.lfd, (struct sockaddr *)&a, sizeof(a)) || listen(S.lfd, 1)) { perror("listen"); exit(2); }
  if (tinynv_pci_open_tinygpu(S.path, &pci)) { printf("open: %s\n", tinynv_last_error()); exit(2); }
  if ((S.srv = accept(S.lfd, NULL, NULL)) < 0) { perror("accept"); exit(2); }
  S.mode = M_NORMAL; S.n = 0; S.nmem = 0; S.cfg_cmd = 0x0407;
  pthread_create(&S.th, NULL, serve, NULL);
  memset(&g, 0, sizeof(g));
  g.dev.pci = &pci;
  if (pci.bar_map(&pci, 0, 0, 0, &g.dev.mmio) || pci.bar_map(&pci, 1, 0, 0, &g.dev.vram)) { printf("bar_map: %s\n", tinynv_last_error()); exit(2); }
  g.gsp.fifo_mem = (tinynv_vmap_t){.va = 0x200000, .size = 0x10000, .nranges = 1, .ranges = &fifo_range};
  q = (tinynv_queue_t){.token = 0x33, .entries = 16, .ring_va = 0x200000, .gpput_off = 0x8000};
  memset(&ex, 0, sizeof(ex));
  memset(sem_host, 0, sizeof(sem_host));
  ex.g = &g;
  ex.sem.dma = (tinynv_dma_t){.va = sem_host, .size = sizeof(sem_host)};
  tinynv_dma_bind_host(&ex.sem.dma);
  ex.q_last[0] = 5;
}
static void session_close(void) {
  if (pci.close) pci.close(&pci);
  pthread_join(S.th, NULL);   // the close is its end of file
  close(S.srv); close(S.lfd);
  unlink(S.path); rmdir(S.dir);
}

static void *later(void *arg) {
  double *at = arg;
  struct timespec ts = {0, (long)(at[0] * 1e9)};
  nanosleep(&ts, NULL);
  if (at[1] >= 0) sem_set(0, (uint64_t)at[1]);    // the engine releases the value
  else { S.mode = M_GONE; shutdown(S.srv, SHUT_WR); }   // the server goes
  return NULL;
}
static pthread_t after(double s, double what) {
  static double args[8][2]; static int k;
  double *a = args[k++ % 8];
  a[0] = s; a[1] = what;
  pthread_t t; pthread_create(&t, NULL, later, a);
  return t;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);

  // ---- short waits and an ordinary announcement: nothing extra on the socket ----
  session_open();
  int n0 = logged();
  sem_set(0, 5);
  double t0 = now();
  CHECK(tinynv_exec_wait(&ex, 5, 1.5) == 0, "a wait already satisfied failed: %s", tinynv_last_error());
  CHECK(logged() == n0, "a wait already satisfied sent %d requests", logged() - n0);
  ex.q_last[0] = 6;   // the timeline only moves forward: 5 now, 6 when released
  pthread_t th = after(0.1, 6);
  t0 = now();
  CHECK(tinynv_exec_wait(&ex, 6, 1.5) == 0, "a 100 ms wait failed: %s", tinynv_last_error());
  pthread_join(th, NULL);
  CHECK(now() - t0 < 0.24, "a 100 ms wait took %.3f s", now() - t0);
  CHECK(logged() == n0, "a 100 ms wait sent %d requests - a wait inside 250 ms must cost nothing extra", logged() - n0);
  ex.ce_route = 1; ex.ce_reserved = 3;
  sem_set(256, 3);   // SEM_CE
  CHECK(tinynv_exec_wait_ce(&ex, 3, 1.5) == 0 && logged() == n0, "a satisfied copy-channel wait sent %d requests", logged() - n0);
  // a healthy wait past 250 ms: one BOOT_0 read per 250 ms, no config space
  ex.q_last[0] = 7;
  th = after(0.6, 7);
  t0 = now();
  CHECK(tinynv_exec_wait(&ex, 7, 3.0) == 0, "a 600 ms healthy wait failed: %s", tinynv_last_error());
  pthread_join(th, NULL);
  int b0 = count_boot0_since(n0);
  CHECK(b0 >= 1 && b0 <= 3 && logged() - n0 == b0, "a 600 ms wait sent %d requests, %d of them BOOT_0 reads (want 2ish, all BOOT_0)", logged() - n0, b0);
  CHECK(count_since(n0, CMD_CFG_READ) == 0, "a long wait polled config space");
  CHECK(!g.lost, "a healthy card was latched lost (%d)", g.lost);
  // an announcement on a healthy card: entry, pointer, the fence read, the doorbell - and nothing else
  int n1 = logged();
  CHECK(tinynv_submit_stage(&g, &q, 0x1000, 8) == 0 && tinynv_submit_ring(&g) == 0, "an ordinary announcement failed: %s", tinynv_last_error());
  usleep(50000);
  CHECK(count_since(n1, CMD_MMIO_WRITE) == 3 && count_since(n1, CMD_MMIO_READ) == 1 && logged() - n1 == 4,
        "an ordinary announcement sent %d requests (%d writes, %d reads), want 3 writes and the fence read", logged() - n1,
        count_since(n1, CMD_MMIO_WRITE), count_since(n1, CMD_MMIO_READ));
  CHECK(count_boot0_since(n1) == 0 && count_since(n1, CMD_CFG_READ) == 0, "an ordinary announcement probed the card");
  session_close();

  // ---- window lost: the server answers, every BAR read is all ones, config space is fine ----
  session_open();
  S.mode = M_WINDOW;
  ex.ce_route = 1; ex.ce_reserved = 3;
  n0 = logged();
  t0 = now();
  int rc = tinynv_exec_wait(&ex, 5, 1.5);
  double took = now() - t0;
  CHECK(rc != 0, "a wait on a card whose window is lost succeeded");
  CHECK(took < 0.6, "a wait on a lost window took %.2f s to give up - it should take about 0.25 s, not its timeout", took);
  CHECK(g.lost == TINYNV_LOST_WINDOW, "the loss was classified as %d, not the window (%d)", g.lost, TINYNV_LOST_WINDOW);
  CHECK(strstr(tinynv_last_error(), "lost") != NULL, "the wait's error does not say lost: %s", tinynv_last_error());
  CHECK(count_since(n0, CMD_CFG_READ) == 1, "the classification read config space %d times, want once", count_since(n0, CMD_CFG_READ));
  // stopped: nothing staged, nothing rung, every wait refuses at once
  usleep(50000);
  n1 = logged();
  t0 = now();
  CHECK(tinynv_submit_stage(&g, &q, 0x1000, 8) != 0, "a lost gpu accepted a batch");
  CHECK(tinynv_submit_ring(&g) != 0, "a lost gpu accepted an announcement");
  CHECK(tinynv_exec_wait(&ex, 5, 1.5) != 0 && tinynv_exec_wait_ce(&ex, 3, 1.5) != 0, "a wait on a lost gpu succeeded");
  CHECK(now() - t0 < 0.05, "waits on a latched loss took %.3f s, not at once", now() - t0);
  usleep(50000);
  CHECK(logged() == n1, "%d requests (%d writes) reached the socket after the loss was latched", logged() - n1, count_since(n1, CMD_MMIO_WRITE));
  // the close (M7, the transport alive): bus mastering off, read back, no word to GSP-RM, then the socket
  g.gsp.up = 1; pci.live = 1;
  // The unload is refused before its first step. On this zeroed state that first step (the falcon half) would fail
  // with no traffic at all, so the socket cannot tell - its error can: the unload sequence must not have been entered.
  tinynv_fail("before the unload");
  CHECK(tinynv_gpu_unload(&g) != 0 && !strstr(tinynv_last_error(), "fwsec"), "a lost gpu's unload was attempted: %s", tinynv_last_error());
  n1 = logged();
  tinynv_gpu_close(&g);
  usleep(50000);
  int nc = logged() - n1;
  ev_t c[3] = {{0}};
  pthread_mutex_lock(&S.mu); for (int i = 0; i < 3 && i < nc; i++) c[i] = S.log[n1 + i]; pthread_mutex_unlock(&S.mu);
  CHECK(nc == 3 && c[0].cmd == CMD_CFG_READ && c[0].off == 4 && c[1].cmd == CMD_CFG_WRITE && c[1].off == 4 && c[1].val == 0x0403 &&
        c[2].cmd == CMD_CFG_READ && c[2].off == 4, "the lost gpu's close sent %d requests, not read / write 0x0403 / read-back", nc);
  CHECK(count_since(n1, CMD_MMIO_WRITE) == 0, "the lost gpu's close wrote to the card (an unload rpc or doorbell)");
  pci.close(&pci); pci.close = NULL;
  pthread_join(S.th, NULL);
  CHECK(S.n == n1 + 4 && S.log[n1 + 3].cmd == EV_EOF, "the socket did not close right after the bus-master clear (%d events)", S.n - n1);
  close(S.srv); close(S.lfd); unlink(S.path); rmdir(S.dir);

  // ---- the announcement's own read-back finds the loss, before any doorbell ----
  session_open();
  CHECK(tinynv_submit_stage(&g, &q, 0x1000, 8) == 0, "staging on a healthy card failed");
  usleep(20000);
  S.mode = M_WINDOW;
  n1 = logged();
  CHECK(tinynv_submit_ring(&g) != 0, "an announcement whose fence read came back all ones was rung");
  usleep(50000);
  CHECK(count_since(n1, CMD_MMIO_WRITE) == 0, "a doorbell went out after the fence read came back all ones");
  CHECK(g.lost == TINYNV_LOST_WINDOW, "the fence read's all ones was not classified as a lost window (%d)", g.lost);
  session_close();

  // ---- server gone mid-wait, and the emergency bus-master clear it starts ----
  char stub[160], out[160];
  session_open();
  snprintf(stub, sizeof(stub), "%s/bmclear-stub.sh", S.dir);
  snprintf(out, sizeof(out), "%s/bmclear-stub.out", S.dir);
  FILE *f = fopen(stub, "w");
  fprintf(f, "#!/bin/sh\necho \"$@\" >> '%s'\n", out);
  fclose(f);
  chmod(stub, 0700);
  setenv("TINYNV_EMERGENCY_BM", "1", 1);
  setenv("TINYNV_BMCLEAR_HELPER", stub, 1);
  setenv("TINYGPU_APP", "/nonexistent/TinyGPU", 1);
  n0 = logged();
  th = after(0.1, -1);
  t0 = now();
  rc = tinynv_exec_wait(&ex, 5, 1.5);
  took = now() - t0;
  pthread_join(th, NULL);
  CHECK(rc != 0, "a wait whose server went away succeeded");
  CHECK(took < 0.6, "a wait whose server went away took %.2f s to give up, not about 0.25 s", took);
  CHECK(g.lost == TINYNV_LOST_SERVER, "the loss was classified as %d, not the server (%d)", g.lost, TINYNV_LOST_SERVER);
  usleep(50000);
  n1 = logged();
  CHECK(tinynv_submit_stage(&g, &q, 0x1000, 8) != 0 && tinynv_submit_ring(&g) != 0, "a gpu with its server gone accepted work");
  tinynv_gpu_close(&g);   // no unload, and the quiesce has no connection to go through
  usleep(50000);
  CHECK(logged() == n1, "%d requests reached the socket after the server was gone", logged() - n1);
  tinynv_gpu_rescue(&g, 3.0);   // the one started at the latch, waited for
  tinynv_gpu_rescue(&g, 3.0);   // and never a second
  char line[512] = "", want[256];
  f = fopen(out, "r");
  int lines = 0;
  if (f) { while (fgets(line, sizeof(line), f)) lines++; fclose(f); }
  snprintf(want, sizeof(want), "--sock %s --old-pid %d\n", S.path, (int)getpid());
  CHECK(lines == 1 && !strcmp(line, want), "the emergency clear ran %d times with \"%s\", want once with \"%s\"", lines, line, want);
  unlink(stub); unlink(out);
  unsetenv("TINYNV_BMCLEAR_HELPER");
  setenv("TINYNV_EMERGENCY_BM", "0", 1);
  session_close();

  printf("test_lost: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

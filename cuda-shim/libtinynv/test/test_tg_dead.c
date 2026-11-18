// The TinyGPU transport after its server goes away (M1 of the 2026-11-18 DART panic analysis), with no card and no
// server: this process listens on a unix socket itself, lets the backend connect to it, and plays the server by
// writing each reply before the call that reads it. Then it shuts its sending side, which is what a dead server looks
// like to the client: the next reply is end of file. What has to hold afterwards:
//   - the loss is latched (pci.transport_dead says so), so a caller can tell "server gone" from "device read all ones";
//   - every later call fails at once and puts not one byte on the socket (the server side here can still read, so a
//     request sent after the loss is SEEN, not inferred);
//   - the loss is said once on stderr, however many calls fail after it;
//   - a split read's error reply keeps the server's message instead of dropping it.
#include "tinynv_pci.h"
#include "gpu.h"
#include "submit.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#pragma pack(push, 1)
typedef struct { uint8_t cmd; uint32_t dev_id, bar; uint64_t arg0, arg1, arg2; } req_t;
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)
enum { CMD_MAP_BAR = 1, CMD_CFG_READ = 3, CMD_CFG_WRITE = 4, CMD_MMIO_READ = 6 };

static void reply(int srv, uint8_t status, uint64_t r0, uint64_t r1, const void *tail, size_t n) {
  resp_t rp = {.status = status, .resp0 = r0, .resp1 = r1};
  if (write(srv, &rp, sizeof(rp)) != (ssize_t)sizeof(rp) || (n && write(srv, tail, n) != (ssize_t)n)) { perror("reply"); exit(2); }
}
// what the client has sent since the last call, without waiting for more
static uint8_t g_sent[4096];
static size_t drain(int srv, uint8_t *first_cmd) {
  uint8_t *buf = g_sent;
  size_t total = 0;
  for (;;) {
    ssize_t n = recv(srv, buf + total, sizeof(g_sent) - total, MSG_DONTWAIT);
    if (n <= 0) break;
    if (!total && first_cmd) *first_cmd = buf[0];
    total += (size_t)n;
  }
  return total;
}

// submit.c's write-pointer check, which on 2026-11-18 called the transport's sentinel "the device": with the backend's
// transport_dead set it must say the server is gone, and with it clear keep its old words
static int g_dead;
static uint32_t fake_recv(tinynv_mmio_t *m) { return 0xffffffffu; }
static int fake_dead(tinynv_pci_t *p) { return g_dead; }
static void wp_check(int dead, int errfd) {
  static tinynv_gpu_t g;
  static tinynv_pci_t pci;
  memset(&g, 0, sizeof(g));
  pci = (tinynv_pci_t){.transport_dead = fake_dead};
  g.dev.pci = &pci;
  g.dev.vram = (tinynv_mmio_t){.rd32_recv = fake_recv};
  g.ring_pending = 1; g.ring_want = 7; g_dead = dead;
  ftruncate(errfd, 0); lseek(errfd, 0, SEEK_SET);
  // With the transport dead the gpu is latched lost (src/lost.c) and the announcement refused - nothing is rung into a
  // connection that is gone. Alive, on this non-live fake nothing can be classified, and it completes as before.
  int rc = tinynv_submit_ring_complete(&g);
  CHECK(dead ? rc != 0 && g.lost == TINYNV_LOST_SERVER : rc == 0 && !g.lost, "ring complete returned %d with the transport %s (lost %d)",
        rc, dead ? "dead" : "alive", g.lost);
  fflush(stderr);
  char log[2048] = {0};
  pread(errfd, log, sizeof(log) - 1, 0);
  int says_server = strstr(log, "tinygpu server connection is lost") != NULL, says_device = strstr(log, "is not reaching the device") != NULL;
  CHECK(strstr(log, "read back as 4294967295 where 7") != NULL, "no write-pointer line: %s", log);
  CHECK(dead ? (says_server && !says_device) : (says_device && !says_server), "transport %s, the line reads: %s", dead ? "dead" : "alive", log);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN); // as llama-server does: a send to a dead peer fails with EPIPE rather than killing us
  char dir[] = "/tmp/tgdead.XXXXXX", path[128], errpath[160];
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
  snprintf(path, sizeof(path), "%s/tinygpu.sock", dir);
  snprintf(errpath, sizeof(errpath), "%s/stderr.log", dir);

  // stderr to a regular file, as tools/serve.sh sends the client's: the loss line is counted from it, and the sync
  // after the line takes its regular-file path
  int errfd = open(errpath, O_RDWR | O_CREAT | O_TRUNC, 0600), saved = dup(2);
  if (errfd < 0 || saved < 0 || dup2(errfd, 2) < 0) { perror("stderr"); return 2; }

  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 1)) { perror("listen"); return 2; }

  tinynv_pci_t pci;
  if (tinynv_pci_open_tinygpu(path, &pci)) { fprintf(stderr, "open: %s\n", tinynv_last_error()); return 2; }
  int srv = accept(lfd, NULL, NULL); // the connect above completed from the backlog; this is its other end
  if (srv < 0) { perror("accept"); return 2; }
  uint8_t cmd = 0xff;

  // a live session first: one config read and one bar window, each answered
  reply(srv, 0, 0x2504, 0, NULL, 0);
  CHECK(pci.cfg_read(&pci, 2, 2) == 0x2504, "a config read the server answered");
  CHECK(drain(srv, &cmd) == sizeof(req_t) && cmd == CMD_CFG_READ, "the config read went out as one request");
  reply(srv, 0, 0, 16 << 20, NULL, 0);
  tinynv_mmio_t regs;
  CHECK(pci.bar_map(&pci, 0, 0, 0, &regs) == 0, "map bar0: %s", tinynv_last_error());
  CHECK(drain(srv, &cmd) == sizeof(req_t) && cmd == CMD_MAP_BAR, "the bar map went out as one request");
  CHECK(pci.transport_dead != NULL, "the backend has no transport_dead, so a caller cannot tell a lost server from all ones");
  CHECK(!pci.transport_dead || !pci.transport_dead(&pci), "a live connection reads as dead");

  // the quiesce on a LIVE connection still clears bus mastering: read PCI_COMMAND, write it back without MASTER, read
  // it back. The latch must not have turned this into a no-op - it is the one thing that stops a card's DMA.
  reply(srv, 0, 0x0407, 0, NULL, 0);
  reply(srv, 0, 0, 0, NULL, 0);
  reply(srv, 0, 0x0403, 0, NULL, 0);
  pci.quiesce(&pci);
  size_t q = drain(srv, &cmd);
  req_t qr[3];
  if (q == sizeof(qr)) memcpy(qr, g_sent, sizeof(qr));
  CHECK(q == sizeof(qr) && qr[0].cmd == CMD_CFG_READ && qr[0].arg0 == 4 && qr[1].cmd == CMD_CFG_WRITE && qr[1].arg0 == 4 &&
        qr[1].arg2 == 0x0403 && qr[2].cmd == CMD_CFG_READ, "the live quiesce sent %zu bytes, not read / write 0x0403 / read-back", q);

  // a split read whose reply is an error: the server's words must survive, not be dropped on the floor
  nv_rd32_send(&regs, 0x40);
  const char why[] = "bar 0 offset out of range";
  reply(srv, 1, sizeof(why) - 1, 0, why, sizeof(why) - 1);
  CHECK(nv_rd32_recv(&regs) == 0xffffffffu, "an error reply reads as the sentinel");
  CHECK(strstr(tinynv_last_error(), why) != NULL, "the split read's error text was discarded (last error: %s)", tinynv_last_error());
  CHECK(!pci.transport_dead || !pci.transport_dead(&pci), "an error REPLY is not a lost connection");
  drain(srv, NULL);

  // the server goes: its side stops sending (and can still read, so whatever the client sends next is seen)
  shutdown(srv, SHUT_WR);
  CHECK(pci.cfg_read(&pci, 0, 4) == 0xffffffffu, "a read with the server gone reads as the sentinel");
  CHECK(drain(srv, &cmd) == sizeof(req_t), "the read that discovered the loss sent its request");
  CHECK(pci.transport_dead && pci.transport_dead(&pci), "end of file did not latch the loss");
  CHECK(strstr(tinynv_last_error(), "tinygpu server connection lost") != NULL, "last error: %s", tinynv_last_error());

  // everything after it: fails, and not one byte reaches the socket
  uint32_t blk[4] = {1, 2, 3, 4}, out[4] = {0};
  uint64_t size = 0;
  tinynv_dma_t dma;
  CHECK(pci.cfg_read(&pci, 4, 2) == 0xffffffffu, "config read after the loss");
  pci.cfg_write(&pci, 4, 2, 0x0403);
  CHECK(pci.bar_info(&pci, 1, NULL, &size) != 0, "bar_info after the loss succeeded");
  CHECK(nv_rd32(&regs, 0x40) == 0xffffffffu, "mmio read after the loss");
  nv_wr32(&regs, 0x40, 0xdeadbeef);
  nv_wr_block(&regs, 0x80, blk, sizeof(blk));
  nv_rd_block(&regs, 0x80, out, sizeof(out));
  CHECK(out[0] == 0xffffffffu && out[3] == 0xffffffffu, "block read after the loss is not all ones");
  nv_rd32_send(&regs, 0x40);
  CHECK(nv_rd32_recv(&regs) == 0xffffffffu, "split read recv after the loss");
  CHECK(pci.dma_alloc(&pci, 0x4000, &dma) != 0, "dma_alloc after the loss succeeded");
  CHECK(pci.reset(&pci) != 0, "reset after the loss succeeded");
  // the quiesce cannot reach config space without a server, today or after this change: it must read, see the
  // sentinel and leave, as before - not write a command register through a connection that is gone
  pci.quiesce(&pci);
  size_t after = drain(srv, &cmd);
  CHECK(after == 0, "%zu bytes (first command %u) went to the socket after the loss was latched", after, cmd);
  CHECK(pci.transport_dead && pci.transport_dead(&pci), "the loss did not stay latched");
  CHECK(strstr(tinynv_last_error(), "tinygpu server connection lost") != NULL, "last error after the loss: %s", tinynv_last_error());
  pci.close(&pci);

  // said once
  fflush(stderr);
  char log[8192] = {0};
  ssize_t n = pread(errfd, log, sizeof(log) - 1, 0);
  wp_check(1, errfd);
  wp_check(0, errfd);
  dup2(saved, 2);
  int said = 0;
  for (const char *s = log; n > 0 && (s = strstr(s, "tinygpu server connection lost")); s++) said++;
  CHECK(said == 1, "the loss was said %d times on stderr, not once", said);
  CHECK(strstr(log, "split mmio read failed at the server: bar 0 offset out of range") != NULL, "the split read's error never reached stderr");
  if (said != 1 || fails) fprintf(stderr, "--- stderr of the run ---\n%s---\n", log);

  close(srv); close(lfd); close(errfd);
  unlink(path); unlink(errpath); rmdir(dir);
  printf("test_tg_dead: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

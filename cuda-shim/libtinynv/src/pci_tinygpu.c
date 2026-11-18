// TinyGPU backend: on macOS a signed helper owns the DriverKit user client, so config space, BAR windows and DMA memory are
// reached over a unix socket. Wire format and semantics: docs/driver/architecture.md §2 (identical to the python client in
// tinygrad/runtime/support/system.py, so the two can be diffed). Writes are posted and carry no reply; reads are round trips.
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
  // A read whose request has been sent and whose reply has not been taken. Any other request must first take that
  // reply (the socket carries one conversation), so rpc() drains it into pending_val and rd32_recv finds it there.
  int pending, pending_done;
  uint32_t pending_val;
  int ndma;
  struct { void *va; size_t size; } dma[MAX_DMA];
  uint64_t bar_size[TINYNV_MAX_BARS];
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

static int xfer_all(int fd, void *buf, size_t len, int sending) {
  for (size_t off = 0; off < len;) {
    ssize_t n = sending ? send(fd, (char *)buf + off, len - off, 0) : recv(fd, (char *)buf + off, len - off, 0);
    if (n <= 0) return tinynv_fail("tinygpu: connection closed after %zu of %zu bytes", off, len);
    off += (size_t)n;
  }
  return 0;
}

// Take the reply of a split read that is still on the socket: the header, an error message if it carries one, and
// the four bytes read. Kept for rd32_recv, which may be asked for it after other requests have gone by.
static uint32_t tg_take_pending(tg_t *tg) {
  resp_t rp;
  uint32_t v = 0xffffffffu;
  tg->pending = 0;
  if (xfer_all(tg->fd, &rp, sizeof(rp), 0)) return v;
  if (rp.status != 0) {
    char msg[256];
    size_t n = rp.resp0 < sizeof(msg) ? (size_t)rp.resp0 : sizeof(msg);
    if (n) xfer_all(tg->fd, msg, n, 0);
    return v;
  }
  xfer_all(tg->fd, &v, 4, 0);
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
  if (xfer_all(tg->fd, &rq, sizeof(rq), 1)) return -1;
  if (payload_len && xfer_all(tg->fd, (void *)payload, payload_len, 1)) return -1;

  if (fd_out) { // the reply carries a descriptor for the shared memory the server just created
    *fd_out = -1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = rp, .iov_len = sizeof(*rp)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    ssize_t n = recvmsg(tg->fd, &msg, 0);
    if (n != (ssize_t)sizeof(*rp)) return tinynv_fail("tinygpu: short reply (%zd bytes)", n);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd_out, CMSG_DATA(c), sizeof(int));
  } else if (xfer_all(tg->fd, rp, sizeof(*rp), 0)) return -1;

  if (rp->status != 0) { // the message follows the reply, resp0 bytes of it
    char msg[256] = "unknown error";
    size_t n = rp->resp0 < sizeof(msg) - 1 ? (size_t)rp->resp0 : sizeof(msg) - 1;
    if (n && !xfer_all(tg->fd, msg, n, 0)) msg[n] = 0;
    return tinynv_fail("tinygpu: command %u failed: %s", cmd, msg);
  }
  if (readout_len && xfer_all(tg->fd, readout, readout_len, 0)) return -1;
  if (t0) atomic_fetch_add_explicit(&g_rwait_ns, rs_now() - t0, memory_order_relaxed);
  return 0;
}

static uint32_t tg_cfg_read(tinynv_pci_t *p, uint32_t off, uint32_t size) {
  resp_t rp;
  if (rpc(p->ctx, &rp, CMD_CFG_READ, 0, off, size, 0, NULL, 0, NULL, 0, NULL)) return 0xffffffff;
  return (uint32_t)rp.resp0;
}

static void tg_cfg_write(tinynv_pci_t *p, uint32_t off, uint32_t size, uint32_t val) {
  resp_t rp;
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
  if (xfer_all(tg->fd, &rq, sizeof(rq), 1)) return -1;
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
  if (!xfer_all(tg->fd, &rq, sizeof(rq), 1)) xfer_all(tg->fd, &v, 4, 1);
}
static void tg_rd_block(tinynv_mmio_t *m, uint64_t off, void *dst, size_t n) {
  resp_t rp;
  if (rpc(m->ctx, &rp, CMD_MMIO_READ, (uint32_t)m->bar, m->off + off, n, 0, NULL, 0, dst, n, NULL)) memset(dst, 0xff, n);
}
static void tg_wr_block(tinynv_mmio_t *m, uint64_t off, const void *src, size_t n) {
  tg_t *tg = m->ctx;
  req_t rq = {.cmd = CMD_MMIO_WRITE, .bar = (uint32_t)m->bar, .arg0 = m->off + off, .arg1 = n};
  rs_count(CMD_MMIO_WRITE, RK_POSTED, n); rs_off(1, (uint32_t)m->bar, m->off + off);
  if (!xfer_all(tg->fd, &rq, sizeof(rq), 1)) xfer_all(tg->fd, (void *)src, n, 1);
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

static void tg_quiesce(tinynv_pci_t *p) {
  uint32_t cmd = tg_cfg_read(p, PCI_COMMAND, 2);
  if (cmd == 0xffffffff) return; // already gone from the bus
  tg_cfg_write(p, PCI_COMMAND, 2, cmd & ~PCI_COMMAND_MASTER);
  tg_cfg_read(p, PCI_COMMAND, 2); // read back, so the write has landed before any mapping is torn down
}

static void tg_close(tinynv_pci_t *p) {
  tg_t *tg = p->ctx;
  if (!tg) return;
  for (int i = 0; i < tg->ndma; i++) munmap(tg->dma[i].va, tg->dma[i].size);
  if (tg->fd >= 0) close(tg->fd);
  free(tg);
  p->ctx = NULL;
}

int tinynv_pci_open_tinygpu(const char *sock_path, tinynv_pci_t *out) {
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(sock_path) >= sizeof(addr.sun_path)) return tinynv_fail("tinygpu: socket path is too long");
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return tinynv_fail("tinygpu: socket: %s", strerror(errno));
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
    close(fd);
    return tinynv_fail("tinygpu: no server on %s: %s", sock_path, strerror(errno));
  }
  int buf = 64 << 20;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

  tg_t *tg = calloc(1, sizeof(tg_t));
  tg->fd = fd;
  *out = (tinynv_pci_t){.ctx = tg, .live = 1, .name = "usb4", .cfg_read = tg_cfg_read, .cfg_write = tg_cfg_write, .bar_info = tg_bar_info,
                        .bar_map = tg_bar_map, .bar_unmap = tg_bar_unmap, .dma_alloc = tg_dma_alloc, .dma_free = tg_dma_free,
                        .reset = tg_reset, .quiesce = tg_quiesce, .close = tg_close};
  return 0;
}

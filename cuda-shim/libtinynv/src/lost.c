// The gpu is lost: what the 2026-11-18 DART panic analysis calls M2. That night the TinyGPU server's session ended
// mid-upload, every read came back as the transport's 0xffffffff, and the driver went on staging work and ringing
// doorbells, then sat out 30 s waits on semaphores the card could no longer write. Here the first impossible value is
// classified and latched, and from then on nothing is submitted and every wait fails at once.
//
// Three kinds, told apart from the cheapest evidence up:
//   server lost  the transport has latched its own loss (pci_tinygpu.c): no read has reached the card since
//   card lost    the transport answers, but config space reads all ones: the function is not on the bus
//   window lost  config space answers, but BAR0's NV_PMC_BOOT_0 reads all ones: the card's decode was cleared under
//                it (2026-09-21), so it is on the bus and nothing through its windows reaches it
// Only a live backend is asked anything: a recording has no answer to an operation it never recorded, so on replay
// the only evidence used is the transport's latch, which costs nothing.
//
// Depends on the pci vtable and err.c alone, so the teardown tests that link err.o by itself can link this too.
#include "gpu.h"
#include "internal.h"
#include "nv_regs.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *kind_name(int k) {
  return k == TINYNV_LOST_SERVER ? "the tinygpu server is gone" : k == TINYNV_LOST_CARD ? "the card is off the bus"
       : k == TINYNV_LOST_WINDOW ? "the card's BAR window is lost" : "not lost";
}

static int transport_dead(tinynv_pci_t *pci) { return pci && pci->transport_dead && pci->transport_dead(pci); }

int tinynv_gpu_lost_fail(tinynv_gpu_t *g) {
  return tinynv_fail("the gpu is lost (%s): nothing more is submitted to it or waited for", kind_name(g->lost));
}

void tinynv_gpu_rescue(tinynv_gpu_t *g, double wait_s) {
  tinynv_pci_t *pci = g->dev.pci;
  if (pci && pci->rescue) pci->rescue(pci, wait_s);
}

// Latch, say it once, push the line to disk (as tg_lose does: a panic 32 s later took the unsynced tail of the log),
// and for a lost server start the bus-master clear at once rather than at teardown - the fault storm is running now.
static int latch(tinynv_gpu_t *g, int kind, const char *found_by) {
  if (g->lost || !kind) return g->lost;
  g->lost = kind;
  fprintf(stderr, "libtinynv: GPU LOST: %s (found by %s). Nothing more is staged or announced and every wait fails at "
          "once; %s\n", kind_name(kind), found_by,
          kind == TINYNV_LOST_SERVER ? "the card may still be bus mastering, so a separate process clears that through "
                                       "a fresh server connection (TINYNV_EMERGENCY_BM=0 turns it off)"
                                     : "the connection still works, so bus mastering is cleared at teardown, before it closes");
  fflush(stderr);
  struct stat st;
  if (fstat(2, &st) == 0 && S_ISREG(st.st_mode)) {
#ifdef F_FULLFSYNC
    if (fcntl(2, F_FULLFSYNC) == -1)
#endif
      fsync(2);
  }
  if (kind == TINYNV_LOST_SERVER) tinynv_gpu_rescue(g, 0);
  return kind;
}

int tinynv_gpu_note_transport(tinynv_gpu_t *g, const char *found_by) {
  if (!g->lost && transport_dead(g->dev.pci)) latch(g, TINYNV_LOST_SERVER, found_by);
  return g->lost;
}

int tinynv_gpu_lose(tinynv_gpu_t *g, const char *found_by, int b0_all_ones) {
  tinynv_pci_t *pci = g->dev.pci;
  if (g->lost) return g->lost;
  if (transport_dead(pci)) return latch(g, TINYNV_LOST_SERVER, found_by);
  if (!pci || !pci->live) return 0;
  if (!b0_all_ones) {
    uint32_t b0 = nv_rd32(&g->dev.mmio, NV_PMC_BOOT_0);
    if (transport_dead(pci)) return latch(g, TINYNV_LOST_SERVER, found_by);
    if (b0 != 0xffffffffu) return 0;   // BAR0 answers: whatever read all ones, the card is reachable
  }
  // Config space only now, after BAR0 has read all ones: one read on a path that is already failing, never a poll.
  uint32_t vid = pci->cfg_read(pci, 0x00, 2);
  if (transport_dead(pci)) return latch(g, TINYNV_LOST_SERVER, found_by);
  return latch(g, (vid & 0xffff) == 0xffff ? TINYNV_LOST_CARD : TINYNV_LOST_WINDOW, found_by);
}

int tinynv_gpu_probe(tinynv_gpu_t *g, const char *found_by) {
  if (g->lost) return tinynv_gpu_lost_fail(g);
  tinynv_pci_t *pci = g->dev.pci;
  if (tinynv_gpu_note_transport(g, found_by)) return tinynv_gpu_lost_fail(g);
  if (!pci || !pci->live) return 0;
  // The semaphores a wait spins on are host memory, so with a dead server nothing in the spin touches the socket and
  // nothing would ever notice. This read is what finds it: the round trip hits end of file and the transport latches.
  if (nv_rd32(&g->dev.mmio, NV_PMC_BOOT_0) != 0xffffffffu && !transport_dead(pci)) return 0;
  return tinynv_gpu_lose(g, found_by, !transport_dead(pci)) ? tinynv_gpu_lost_fail(g) : 0;
}

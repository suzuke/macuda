// A GPU, as the driver holds it: the chip, its memory, and the two firmware blocks that have to come up in order.
#ifndef TINYNV_GPU_H
#define TINYNV_GPU_H
#include "dev.h"
#include "flcn.h"
#include "gsp.h"
#include "mmu.h"

struct tinynv_gpu {
  tinynv_dev_t dev;
  tinynv_mm_t mm;
  // Whether the read before the doorbell has been confirmed to reach the device; see tinynv_submit. On a backend where
  // that read is a message rather than a load from a mapped bar, it is worth knowing rather than assuming.
  int submit_checked;
  // Batches whose ring entry and write pointer are written but whose doorbell has not been rung. The read that orders
  // those writes ahead of the doorbell is one round trip, and one read fences every write issued before it on that
  // path - so several batches can be staged and then announced together, paying one round trip instead of one each.
  // Two is what the flush path needs (the descriptor delivery and the chain it feeds); four is room to spare.
  tinynv_queue_t *staged[64];
  int nstaged;
  // An announcement in flight: the fence read has been sent, the doorbells wait for its reply. At most one, and the
  // next send or any wait completes it. See tinynv_submit_ring_send / _complete.
  int ring_pending, ring_n;
  uint32_t ring_want;
  tinynv_queue_t *ring_q[64];
  // Held until a cold reset or bus-master quiesce makes SEC2 DMA impossible.
  struct {
    tinynv_bootmem_t image, payload, meta;
    int pending, retry, verify_after_reset;
    uint32_t expected_ss0, expected_ss1;
  } tensor;
  tinynv_flcn_t flcn;
  tinynv_gsp_t gsp;
  // THE GPU IS LOST, latched and never cleared (2026-11-18 DART panic, M2): one of TINYNV_LOST_*. From here nothing is
  // staged, no doorbell is rung, no rpc is sent to GSP-RM, and every wait fails at once. See lost.c.
  int lost;
};

enum { TINYNV_LOST_SERVER = 1, TINYNV_LOST_CARD, TINYNV_LOST_WINDOW };
// An impossible value was read: find out why, and latch the loss if it is one. Returns the TINYNV_LOST_* found (and
// then says it once on stderr and, for a lost server, starts the emergency bus-master clear), or 0 when the card
// still answers. b0_all_ones: the caller has just read NV_PMC_BOOT_0 as all ones, so it is not read again.
int tinynv_gpu_lose(tinynv_gpu_t *g, const char *found_by, int b0_all_ones);
// The health check of a long wait: free when the transport has already latched its loss, otherwise one BAR0 read.
// The waits call it only after 250 ms and then every 250 ms, so a wait that ends sooner costs nothing extra. -1 = lost.
int tinynv_gpu_probe(tinynv_gpu_t *g, const char *found_by);
// The refusal every entry point returns once the loss is latched.
int tinynv_gpu_lost_fail(tinynv_gpu_t *g);
// Latch a server loss the transport has already seen, at no cost (no bus operation). Returns g->lost.
int tinynv_gpu_note_transport(tinynv_gpu_t *g, const char *found_by);
// The emergency bus-master clear through the backend (M3), if it has one; wait_s as tinynv_pci_t.rescue.
void tinynv_gpu_rescue(tinynv_gpu_t *g, double wait_s);

// Identify the chip, put it in a known state, and carve up its memory. Reads and writes registers; runs no firmware.
int tinynv_gpu_open(tinynv_gpu_t *g, tinynv_pci_t *pci);

// Prepare everything both firmware blocks need in memory. No hardware is touched, so this is safe to run anywhere.
int tinynv_gpu_init_sw(tinynv_gpu_t *g);

// Start the firmware: the chain of trust, then waiting for GSP-RM to say it is up. This talks to the GPU.
int tinynv_gpu_init_hw(tinynv_gpu_t *g);

// Both halves, and on a live vbios-path card up to three attempts: a boot whose start-up notice never comes because
// the card's configuration was cleared under the firmware is torn down (falcon reset, FWSEC-SB, booter_unload) and
// made again from the software init on. What device_boot calls; the tests call the halves.
int tinynv_gpu_boot_firmware(tinynv_gpu_t *g);

// Leave the card the way NVIDIA's driver does: GSP-RM told, halted, and its region torn down. tinynv_gpu_close does
// this first unless TINYNV_UNLOAD=0. Returns 0 when there was nothing to unload or it succeeded.
int tinynv_gpu_unload(tinynv_gpu_t *g);
void tinynv_gpu_close(tinynv_gpu_t *g);

#endif

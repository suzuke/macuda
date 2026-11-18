// Can a copy on a copy engine of its own run WHILE a kernel runs? (card probe; built by `make test`, never run by it)
//
// WHY. MoE prefill with experts in host memory measures as transfer + compute, fully serialized. Every copy this driver
// issues goes out on gsp->copy_q, a channel inside the GRAPHICS channel group, and gsp.c's comment (the TINYNV_CE_GROUP
// block in tinynv_gsp_open_client) says that copy engine shares PBDMA 0 and runlist 0 with GR0, "so a copy and a kernel
// cannot be in flight at once". TINYNV_CE_GROUP=<engine type> makes the driver allocate a SECOND channel group on that
// copy engine (SYNC subcontext), a real channel in it (gsp->ce_copy_q, with its own DMA_COPY object) and schedule the
// group - and nothing has ever submitted to it. Before anyone builds routing and cross-channel ordering on top of that
// channel, this asks the one question that decides whether it is worth building: does a copy on it actually overlap a
// long kernel on the compute channel? And, as the control, does a copy on the graphics-group copy_q serialize with it?
//
// WHICH ENGINE TYPE. The value is an NV2080_ENGINE_TYPE ordinal (cl2080_notification.h: COPY0 = 0x9, COPY1 = 0xa,
// COPY2 = 0xb, ...; nv_structs.h says the same next to tinynv_dma_copy_alloc_t). The gsp.c comment was written against
// the 5090, where CE1 is on runlist 1 with PBDMAs [8,9]. The RTX 3060 (GA106) is NOT laid out that way - its engine
// table, as every 3060 boot in logs/ prints it (e.g. logs/prefill-sweep/server-20261118-120251.log):
//     GR0 type 0x1 runlist 0 PBDMAs [0,1]     CE0 type 0x9 runlist 0 PBDMA 0     CE1 type 0xa runlist 0 PBDMA 1
//     CE2 type 0xb runlist 1 PBDMA 5          CE3 type 0xc runlist 2 PBDMA 6     CE4 type 0xd runlist 8 PBDMA 7
// so on the 3060 COPY1 (0xa) sits on GR0's runlist and on GR0's second PBDMA - a GRCE in RM's terms - and a group on it
// would be time-sliced against the graphics group on the same runlist. The first copy engine with a runlist and a PBDMA
// of its own is COPY2, TINYNV_CE_GROUP=0xb. That is the value that answers the question on this card. 0xa is still a
// legitimate second run (a separate group on GR0's runlist); the probe prints the engine table and says which case it is
// in before it measures anything.
//
// WHAT RUNS, each phase RUNS times (3 by default), every interval stamped on the GPU's own clock (globaltimer, ns):
//   P1   the kernel alone on compute_q: libtinycudart's keepalive.sm_86.cubin, tinynv_keepalive(flag, max_cycles), one
//        warp spinning on clock64 with __nanosleep until max_cycles (or until the host sets *flag, which is only done to
//        cut a stalled run short). max_cycles is not assumed: a warm-up launch measures cycles per ns first and the
//        target duration (KERNEL_MS, 1000 by default) is converted with it. The flag is in host memory, so the kernel
//        polls ~4 bytes across the link every couple of microseconds - noise next to a GB/s copy, but it is traffic.
//        Stamped by a front-end timestamp batch just before the launch and a wait-for-idle release just after it.
//   P2   a copy alone on the TINYNV_CE_GROUP channel: one 64 MiB host buffer (halved if the dext refuses 64 MiB) copied
//        N times in ONE batch to a 64 MiB video-memory buffer - the shape of the expert stream. N is calibrated so the
//        copy takes ~60% of the kernel, which leaves a concurrent copy room to sit wholly inside the kernel.
//   P2b  the same, video memory to video memory (N calibrated separately).
//   P2c  the host-to-device copy alone on the graphics-group copy_q (CE0), the control's own baseline.
//   P3   kernel on compute_q, then at once the host-to-device copy on the CE-group channel, nothing ordering them.
//   P3b  the same with the video-memory-to-video-memory copy.
//   P4   CONTROL: P3 with the copy on the graphics-group copy_q instead. Expected SERIALIZED per gsp.c.
// Every copy batch is raw (tinynv_cmd_copy between two copy-engine timestamps) and carries NO semaphore acquire, so
// nothing in software orders it against the kernel: whatever ordering the stamps show is the hardware's. A copy's bytes
// are checked after it (head and tail of the destination against a pattern the run wrote), so a release that landed
// without a transfer cannot pass as a copy.
//
// VERDICTS, per overlap run: intersection of the kernel's and the copy's GPU intervals over the shorter one's duration.
//   CONCURRENT  >= 50%: the copy ran while the kernel ran. A dedicated-CE channel is worth routing expert copies to.
//   SERIALIZED  <  5%: one waited for the other. On P3 that kills the idea for this engine; on P4 it is the expected
//               control. A P4 that is NOT serialized is printed loudly: the premise in gsp.c would then be wrong.
//   PARTIAL     in between: time-sliced (expected for a group sharing GR0's runlist) or a late start; read the stamps.
// The host's own wall clock (CLOCK_MONOTONIC from first submit to last completion seen) is printed beside each overlap
// run as an independent check, and a disagreement with the stamp verdict is printed loudly. A clock-bounded kernel
// cannot stretch, so the evidence is the copy's start against the kernel's interval and its bandwidth alone vs during.
//
// HOW TO RUN (card step; one process; the runner does preflight, the socket and TINYNV_HW=1):
//   cd <worktree> && QUIESCE=1 TINYNV_CE_GROUP=0xb sh tools/nv_shim_step.sh A probe \
//       <worktree>/cuda-shim/build/shim-sm_86/nv/test_hw_ce_overlap [cubin] [kernel_ms=1000] [runs=3]
// TINYNV_CE_GROUP reaches the binary because nv_shim_step.sh runs it with the caller's environment. The default cubin is
// the keepalive.sm_86.cubin of the tree that BUILT this binary (baked in as an absolute path), not of the cwd.
//
// HAZARD. This is the first traffic the TINYNV_CE_GROUP channel has ever carried. Every wait is bounded; a timeout
// releases the kernel (flag = 1), drains briefly, prints both rings' get/put and what the stamps said, submits nothing
// more, and leaves through the runtime's own teardown (this file compiles ../src/tinynv.c, as test_teardown_term does,
// so the boot, the atexit teardown and the signal handlers are exactly the ones libtinycudart gets). A channel that
// hangs is torn down with GSP-RM's unload at exit; that path has never been exercised with a hung copy-engine group, so
// run this with QUIESCE=1 and as the last step of a session.
//
// RESULT, RTX 3060 over Thunderbolt 4, 2026-11-18 (3 runs each, all identical; kernel 999.6 ms, copy ~0.6 s):
//   0xb (CE2, runlist 1)  h2d CONCURRENT 100%, 3.02 GB/s during vs 3.03 alone; d2d CONCURRENT 100%, 46.9 vs 46.9 GB/s
//   0xa (CE1, runlist 0)  h2d CONCURRENT 100%, 1.69 GB/s during vs 3.03 alone; d2d CONCURRENT 100%, 25.9 vs 47.3 GB/s
//   control, copy_q (CE0) SERIALIZED 0%: the copy starts 999.6 ms after the kernel, the kernel not stretched
// So on this card a copy routed to CE2 overlaps a kernel at full bandwidth; CE1 overlaps but is time-sliced against
// GR0 on runlist 0 and loses ~45%. A first run read zeros back: the probe's own 4 KB clear was an inline upload held
// by the runtime (exec.c held list) and only went out with the readback, AFTER the raw CE copy. Anything that routes
// copies to a second channel must flush held uploads first - hence no_held_uploads() below.
#include "../src/tinynv.c"

#include <time.h>

#ifndef TINYNV_CE_OVERLAP_CUBIN
#define TINYNV_CE_OVERLAP_CUBIN "../libtinycudart/keepalive.sm_86.cubin"
#endif

#define MIB (1024ull * 1024ull)

// One host buffer the probe owns: stamps, the kernel's flag, and every command buffer it builds. Host memory because
// the processor writes the batches and reads the stamps, and the engines reach it across the link (exec.c's own
// timeline lives in host memory for the same reason).
#define PM_BYTES      (1u << 20)
// Each stamp is 16 bytes: payload at +0, the engine's clock at +8. None is at offset 0, which wait_tags reads as "no
// stamp to wait for".
#define ST_SETUP      0x020
#define ST_K0         0x040
#define ST_K1         0x080
#define ST_C0         0x0c0
#define ST_C1         0x100
#define ST_BYTES      0x200
#define PM_FLAG       0x800
#define CB_SETUP      0x01000
#define CB_K0         0x02000
#define CB_K1         0x03000
#define CB_SMALL      0x1000
#define CB_COPY       0x10000
#define PM_HOSTDST    0xF0000   // 4 KB a P0 copy may write into: does the CE-group channel's write reach host memory?
#define CB_COPY_BYTES (PM_HOSTDST - CB_COPY)
#define MAX_REPS      4096u   // 9 dwords a repeat: 144 KB of batch at most, well inside CB_COPY_BYTES
#define VERIFY_BYTES  4096u
#define WAIT_S        20.0    // per run; a 3 GiB copy at 1 GB/s still fits

static tinynv_gpu_t *G;
static tinynv_exec_t *EX;
static tinynv_vmap_t pm, hsrc;
static tinynv_devptr_t vA, vB;
static uint64_t chunk;
static tinynv_stream_t S;
static tinynv_kernel_t K;
static tinynv_kernel_info_t KI;
static uint32_t tagseq = 0x100, a_tag;
static int hung;

static double host_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t rd64(uint64_t off) { uint64_t v = 0; nv_rd_block(&pm.dma.view, off, &v, sizeof v); return v; }
static void wr32(uint64_t off, uint32_t v) { nv_wr_block(&pm.dma.view, off, &v, sizeof v); }
static tinynv_cmdbuf_t cb_at(uint64_t off, uint64_t bytes) {
  tinynv_cmdbuf_t c = {(uint32_t *)((uint8_t *)pm.dma.va + off), 0, (uint32_t)(bytes / 4)};
  return c;
}
static int submit_cb(tinynv_queue_t *q, uint64_t off, const tinynv_cmdbuf_t *c) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);   // the batch is in host memory before its ring entry is written
  return tinynv_submit(G, q, pm.va + off, c->n);
}
static uint32_t pattern(uint32_t tag, uint64_t off) { return (tag * 0x9E3779B1u) ^ (uint32_t)(off / 4); }
static const char *engine_name(uint32_t t, char *buf, size_t n) {
  if (t == 0x1) snprintf(buf, n, "GR0");
  else if (t >= 0x9 && t <= 0x12) snprintf(buf, n, "CE%u", t - 0x9);
  else snprintf(buf, n, "other");
  return buf;
}

static void show_queue(const char *name, tinynv_queue_t *q) {
  uint32_t get = 0, put = 0; uint64_t pub = 0;
  if (!q->entries) { printf("    %-10s no ring\n", name); return; }
  tinynv_queue_state(G, q, &get, &put, &pub);
  printf("    %-10s channel %#x token %#x: engine has consumed %u, put %u, driver published %llu\n", name, q->channel,
         q->token, get, put, (unsigned long long)pub);
}
static void report_stall(const char *what) {
  printf("  STALL during %s: stopping, no further submissions\n", what);
  static const struct { uint64_t off; const char *n; } st[] = {
      {ST_SETUP, "setup"}, {ST_K0, "kernel start"}, {ST_K1, "kernel end"}, {ST_C0, "copy start"}, {ST_C1, "copy end"}};
  for (size_t i = 0; i < sizeof st / sizeof *st; i++)
    printf("    stamp %-12s payload %#llx clock %llu\n", st[i].n, (unsigned long long)rd64(st[i].off),
           (unsigned long long)rd64(st[i].off + 8));
  show_queue("compute_q", &G->gsp.compute_q);
  show_queue("copy_q", &G->gsp.copy_q);
  show_queue("ce_copy_q", &G->gsp.ce_copy_q);
  if (tinynv_gsp_poll(G)) printf("    gsp-rm reports an error (see the tinynv lines on stderr)\n");
  hung = 1;
}

// Wait for up to two stamps' payloads to reach `tag`, recording when the host saw each. Bounded, and gsp-rm is asked
// every quarter second whether it has something to say, because a faulted channel never completes on its own.
static int wait_tags(uint64_t off_a, uint64_t off_b, uint32_t tag, double timeout, double *seen_a, double *seen_b) {
  double t0 = host_now(), last_poll = t0;
  int a = !off_a, b = !off_b;
  while (!(a && b)) {
    double t = host_now();
    if (!a && (uint32_t)rd64(off_a) == tag) { a = 1; if (seen_a) *seen_a = t; }
    if (!b && (uint32_t)rd64(off_b) == tag) { b = 1; if (seen_b) *seen_b = t; }
    if (a && b) break;
    if (t - last_poll > 0.25) {
      last_poll = t;
      if (tinynv_gsp_poll(G)) { printf("  gsp-rm reported an error while waiting\n"); return -1; }
    }
    if (t - t0 > timeout) return -1;
    usleep(100);
  }
  return 0;
}

static int kernel_submit(uint64_t cycles, uint32_t tag) {
  wr32(PM_FLAG, 0);
  tinynv_cmdbuf_t c = cb_at(CB_K0, CB_SMALL);
  if (tinynv_cmd_timestamp(&c, pm.va + ST_K0, tag) || submit_cb(&G->gsp.compute_q, CB_K0, &c)) return -1;
  uint8_t params[64];
  memset(params, 0, sizeof params);
  uint64_t fva = pm.va + PM_FLAG;
  memcpy(params + KI.params[0].offset, &fva, 8);
  memcpy(params + KI.params[1].offset, &cycles, 8);
  if (tinynv_launch(S, K, 1, 1, 1, 32, 1, 1, 0, params, (size_t)KI.params[1].offset + 8) != TINYNV_OK) return -1;
  if (WITH_LOCK(tinynv_exec_flush(EX))) return -1;   // hand the chain over now; nothing waits for it
  c = cb_at(CB_K1, CB_SMALL);
  // waits for the compute engine to go idle, then writes the payload and the clock: the kernel's end
  if (tinynv_cmd_release_clocked(&c, pm.va + ST_K1, tag) || submit_cb(&G->gsp.compute_q, CB_K1, &c)) return -1;
  return 0;
}

static int copy_submit(tinynv_queue_t *q, uint64_t dst, uint64_t src, uint64_t bytes, uint32_t reps, uint32_t tag) {
  tinynv_cmdbuf_t c = cb_at(CB_COPY, CB_COPY_BYTES);
  if (tinynv_cmd_copy_timestamp(&c, pm.va + ST_C0, tag)) return -1;
  for (uint32_t r = 0; r < reps; r++)
    if (tinynv_cmd_copy(&c, dst, src, bytes)) return -1;
  if (tinynv_cmd_copy_timestamp(&c, pm.va + ST_C1, tag)) return -1;
  return submit_cb(q, CB_COPY, &c);
}

// The destination's head and tail through the runtime's own download path, after the timed window.
static int verify(tinynv_devptr_t dst, uint64_t bytes, uint32_t tag, const char *what) {
  static uint32_t got[VERIFY_BYTES / 4];
  uint64_t offs[2] = {0, bytes - VERIFY_BYTES};
  for (int h = 0; h < 2; h++) {
    if (tinynv_memcpy_dtoh(S, got, dst + offs[h], VERIFY_BYTES) != TINYNV_OK) {
      printf("  %s: reading the destination back failed: %s\n", what, tinynv_last_error());
      return -1;
    }
    for (uint32_t i = 0; i < VERIFY_BYTES / 4; i++)
      if (got[i] != pattern(tag, offs[h] + 4ull * i)) {
        printf("  %s: destination byte %#llx reads %#x, the copy should have left %#x - the stamps landed without the "
               "bytes\n", what, (unsigned long long)(offs[h] + 4ull * i), got[i], pattern(tag, offs[h] + 4ull * i));
        return -1;
      }
  }
  return 0;
}
static void mark_src(uint32_t tag) {
  uint32_t *p = (uint32_t *)hsrc.dma.va;
  uint64_t offs[2] = {0, chunk - VERIFY_BYTES};
  for (int h = 0; h < 2; h++)
    for (uint32_t i = 0; i < VERIFY_BYTES / 4; i++) p[(offs[h] / 4) + i] = pattern(tag, offs[h] + 4ull * i);
}
// THE 2026-11-18 P0 FAILURE WAS HERE. This used tinynv_memcpy_htod of 4 KB zeros + tinynv_stream_sync. A host-to-device
// copy that small is an INLINE upload by default (exec.c tinynv_exec_inline_upload_mode, on unless =0): it is HELD, not
// sent (exec.c ~2004), and stream_sync does not send it either - flush returns at once with no chain (exec.c:1330) and
// the wait is on the timeline only. The raw copy below then wrote the pattern, and verify's download - the next thing
// through batch_begin - pushed the held zeros out first (exec.c:997), on top of the copy. So: the sentinel goes in
// through tinynv_memset with a NONZERO byte, which is a staging-buffer copy on the copy engine, never inline; and
// no_held_uploads() refuses to submit raw work while anything is still held.
#define SENTINEL_BYTE 0x5A
#define SENTINEL      0x5A5A5A5Au
static int no_held_uploads(const char *where) {
  if (!EX->inline_pend_n) return 0;
  printf("  %s: %u host-to-device uploads are still HELD by the runtime (inline, unsent) - they would land after this "
         "probe's raw copy and overwrite it; refusing to submit\n", where, EX->inline_pend_n);
  return -1;
}
static int fill_sentinel(tinynv_devptr_t dst, uint64_t bytes) {
  if (tinynv_memset(S, dst, SENTINEL_BYTE, VERIFY_BYTES) != TINYNV_OK) return -1;
  if (bytes > VERIFY_BYTES && tinynv_memset(S, dst + bytes - VERIFY_BYTES, SENTINEL_BYTE, VERIFY_BYTES) != TINYNV_OK) return -1;
  return tinynv_stream_sync(S) != TINYNV_OK || no_held_uploads("filling a destination");
}
static int clear_dst(tinynv_devptr_t dst) { return fill_sentinel(dst, chunk); }

// One P0 cell: a 4 KB copy on `q`, its destination pre-filled with the sentinel, and everything about it printed.
// dst_host: the destination is the probe page at PM_HOSTDST, read back through the cpu mapping instead of a download.
// Returns 0 landed, -1 did not (or could not be set up), -2 stalled (the caller stops at once).
static int p0_cell(const char *id, const char *what, tinynv_queue_t *q, uint64_t dst, int dst_host, uint64_t src,
                   int src_is_hsrc, uint32_t expect_tag) {
  static uint32_t got[VERIFY_BYTES / 4];
  uint32_t tag = ++tagseq, get0 = 0, put0 = 0, get1 = 0, put1 = 0;
  uint64_t pub = 0;
  if (dst_host) memset((uint8_t *)pm.dma.va + PM_HOSTDST, SENTINEL_BYTE, VERIFY_BYTES);
  else if (fill_sentinel((tinynv_devptr_t)dst, VERIFY_BYTES)) {
    printf("P0%s  %s: could not pre-fill the destination: %s\n", id, what, tinynv_last_error());
    return -1;
  }
  if (no_held_uploads(id)) return -1;
  memset(pm.dma.va, 0, ST_BYTES);
  tinynv_queue_state(G, q, &get0, &put0, &pub);
  if (src_is_hsrc)
    printf("P0%s  source word 0 through the cpu mapping just before submit: %#x (expect %#x)\n", id,
           ((const uint32_t *)hsrc.dma.va)[0], pattern(expect_tag, 0));
  if (copy_submit(q, dst, src, VERIFY_BYTES, 1, tag)) {
    printf("P0%s  %s: submitting failed: %s\n", id, what, tinynv_last_error());
    return -1;
  }
  if (wait_tags(ST_C1, 0, tag, 5.0, NULL, NULL)) { report_stall(what); return -2; }
  tinynv_queue_state(G, q, &get1, &put1, &pub);
  printf("P0%s  stamps for tag %#x: start payload %#llx clock %llu, end payload %#llx clock %llu (%.1f us); ring "
         "get/put %u/%u before, %u/%u after\n", id, tag, (unsigned long long)rd64(ST_C0), (unsigned long long)rd64(ST_C0 + 8),
         (unsigned long long)rd64(ST_C1), (unsigned long long)rd64(ST_C1 + 8), ((double)rd64(ST_C1 + 8) - (double)rd64(ST_C0 + 8)) / 1e3,
         get0, put0, get1, put1);
  if (dst_host) memcpy(got, (const uint8_t *)pm.dma.va + PM_HOSTDST, VERIFY_BYTES);
  else if (tinynv_memcpy_dtoh(S, got, (tinynv_devptr_t)dst, VERIFY_BYTES) != TINYNV_OK) {
    printf("P0%s  %s: reading the destination back failed: %s\n", id, what, tinynv_last_error());
    return -1;
  }
  unsigned match = 0, sent = 0, zero = 0, other = 0;
  for (uint32_t i = 0; i < VERIFY_BYTES / 4; i++) {
    if (got[i] == pattern(expect_tag, 4ull * i)) match++;
    else if (got[i] == SENTINEL) sent++;
    else if (!got[i]) zero++;
    else other++;
  }
  const char *v = match == VERIFY_BYTES / 4 ? "LANDED" : sent == VERIFY_BYTES / 4 ? "NEVER-WRITTEN (sentinel survives)"
                : zero == VERIFY_BYTES / 4 ? "WRITTEN-WITH-ZEROS (source read as 0)" : "MIXED";
  printf("P0%s  %-48s %s: %u/%u words match, %u sentinel, %u zero, %u other; word 0 %#x\n", id, what, v, match,
         VERIFY_BYTES / 4, sent, zero, other, got[0]);
  return match == VERIFY_BYTES / 4 ? 0 : -1;
}

enum { CP_NONE, CP_CE_H2D, CP_CE_D2D, CP_GFX_H2D };
enum { P1, P2, P2B, P2C, P3, P3B, P4, NPH };
static const struct { const char *tag, *what; int kernel, copy; } PH[NPH] = {
    {"P1 ", "kernel alone on compute_q", 1, CP_NONE},
    {"P2 ", "h2d alone, CE-group channel", 0, CP_CE_H2D},
    {"P2b", "d2d alone, CE-group channel", 0, CP_CE_D2D},
    {"P2c", "h2d alone, graphics copy_q", 0, CP_GFX_H2D},
    {"P3 ", "kernel || h2d on CE-group channel", 1, CP_CE_H2D},
    {"P3b", "kernel || d2d on CE-group channel", 1, CP_CE_D2D},
    {"P4 ", "CONTROL kernel || h2d on graphics copy_q", 1, CP_GFX_H2D},
};
typedef struct { uint64_t k0, k1, c0, c1, bytes; double wall; } run_t;

static uint64_t cycles_for_kernel;
static uint32_t reps_h2d, reps_d2d;

static int one_run(int p, run_t *r, uint32_t reps_override, uint64_t cycles_override) {
  uint32_t tag = ++tagseq;
  int cp = PH[p].copy;
  uint32_t reps = reps_override ? reps_override : (cp == CP_CE_D2D ? reps_d2d : reps_h2d);
  tinynv_queue_t *q = cp == CP_GFX_H2D ? &G->gsp.copy_q : &G->gsp.ce_copy_q;
  tinynv_devptr_t dst = cp == CP_CE_D2D ? vB : vA;
  uint64_t src = cp == CP_CE_D2D ? (uint64_t)vA : hsrc.va;
  uint32_t want = cp == CP_CE_D2D ? a_tag : tag;
  memset(r, 0, sizeof *r);
  if (cp != CP_NONE) {
    if (cp != CP_CE_D2D) mark_src(tag);
    if (clear_dst(dst)) { printf("  could not clear the destination before %s: %s\n", PH[p].tag, tinynv_last_error()); return -1; }
  }
  if (no_held_uploads(PH[p].tag)) return -1;
  memset(pm.dma.va, 0, ST_BYTES);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  double t0 = host_now(), tk = t0, tc = t0;
  if (PH[p].kernel && kernel_submit(cycles_override ? cycles_override : cycles_for_kernel, tag)) {
    printf("  %s: submitting the kernel failed: %s\n", PH[p].tag, tinynv_last_error());
    report_stall("the kernel submission");
    return -1;
  }
  if (cp != CP_NONE && copy_submit(q, dst, src, chunk, reps, tag)) {
    printf("  %s: submitting the copy failed: %s\n", PH[p].tag, tinynv_last_error());
    wr32(PM_FLAG, 1);
    report_stall("the copy submission");
    return -1;
  }
  if (wait_tags(PH[p].kernel ? ST_K1 : 0, cp != CP_NONE ? ST_C1 : 0, tag, WAIT_S, &tk, &tc)) {
    wr32(PM_FLAG, 1);   // let the kernel go, so the compute channel at least can drain
    wait_tags(PH[p].kernel ? ST_K1 : 0, 0, tag, 3.0, NULL, NULL);
    report_stall(PH[p].what);
    return -1;
  }
  r->wall = (tk > tc ? tk : tc) - t0;
  if (PH[p].kernel) {
    if ((uint32_t)rd64(ST_K0) != tag) { printf("  %s: the kernel's start stamp is missing\n", PH[p].tag); return -1; }
    r->k0 = rd64(ST_K0 + 8); r->k1 = rd64(ST_K1 + 8);
    if (tinynv_stream_sync(S) != TINYNV_OK) { printf("  %s: sync after the kernel failed: %s\n", PH[p].tag, tinynv_last_error()); report_stall("the sync"); return -1; }
  }
  if (cp != CP_NONE) {
    if ((uint32_t)rd64(ST_C0) != tag) { printf("  %s: the copy's start stamp is missing\n", PH[p].tag); return -1; }
    r->c0 = rd64(ST_C0 + 8); r->c1 = rd64(ST_C1 + 8);
    r->bytes = chunk * reps;
    if (verify(dst, chunk, want, PH[p].tag)) return -1;
    if (cp != CP_CE_D2D) a_tag = tag;   // vA now holds this run's pattern, which the d2d phases copy onward
  }
  return 0;
}

static double ms(uint64_t a, uint64_t b) { return b > a ? (double)(b - a) / 1e6 : 0.0; }
static double gbs(const run_t *r) { return r->c1 > r->c0 ? (double)r->bytes / (double)(r->c1 - r->c0) : 0.0; }
// intersection over the shorter interval
static double overlap(const run_t *r, double *inter_ms) {
  uint64_t lo = r->k0 > r->c0 ? r->k0 : r->c0, hi = r->k1 < r->c1 ? r->k1 : r->c1;
  double in = hi > lo ? (double)(hi - lo) : 0.0, dk = (double)(r->k1 - r->k0), dc = (double)(r->c1 - r->c0);
  double shorter = dk < dc ? dk : dc;
  if (inter_ms) *inter_ms = in / 1e6;
  return shorter > 0 ? in / shorter : 0.0;
}
static const char *verdict(double f) { return f >= 0.5 ? "CONCURRENT" : f < 0.05 ? "SERIALIZED" : "PARTIAL"; }

static int probe(int argc, char **argv) {
  const char *cubin_path = argc > 1 ? argv[1] : TINYNV_CE_OVERLAP_CUBIN;
  double kernel_ms = argc > 2 ? atof(argv[2]) : 1000.0;
  int runs = argc > 3 ? atoi(argv[3]) : 3;
  if (kernel_ms < 50 || kernel_ms > 5000 || runs < 1 || runs > 10) { printf("  kernel_ms is 50..5000 and runs 1..10\n"); return 2; }
  const char *ceg = getenv("TINYNV_CE_GROUP");
  uint32_t want_et = (uint32_t)strtoul(ceg, NULL, 0);

  FILE *f = fopen(cubin_path, "rb");
  if (!f) { printf("  cannot open the kernel's cubin %s (argument 1 names another)\n", cubin_path); return 1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  void *blob = len > 0 ? malloc((size_t)len) : NULL;
  if (!blob || fread(blob, 1, (size_t)len, f) != (size_t)len) { fclose(f); printf("  cannot read %s\n", cubin_path); return 1; }
  fclose(f);

  printf("libtinynv build %s\n", tinynv_build_id());   // the runner greps this line
  printf("ce-overlap probe: TINYNV_CE_GROUP=%s, kernel %s, target %.0f ms, %d runs a phase\n", ceg, cubin_path, kernel_ms, runs);
  tinynv_device_t dev;
  tinynv_device_props_t props;
  tinynv_module_t mod;
  if (tinynv_init() != TINYNV_OK || tinynv_device_get(&dev, 0) != TINYNV_OK || tinynv_device_props(dev, &props) != TINYNV_OK) {
    printf("  the device did not open: %s\n", tinynv_last_error());
    return 1;
  }
  if (!dev->has_pci || !dev->exec_ready) { printf("  no card behind this device (null device or a failed boot)\n"); return 1; }
  G = &dev->gpu;
  EX = &dev->exec;
  if (tinynv_stream_create(dev, &S) != TINYNV_OK) { printf("  stream: %s\n", tinynv_last_error()); return 1; }
  tinynv_gsp_t *gsp = &G->gsp;
  printf("device %s, sm %d.%d; dma copy class %#x\n", props.name, props.cc_major, props.cc_minor, G->dev.class_dma_copy);

  printf("engine table (type, runlist):\n");
  for (uint32_t i = 0; i < gsp->nengines; i++) {
    char n[16];
    printf("  %-6s type %#4x runlist %u%s%s\n", engine_name(gsp->engines[i], n, sizeof n), gsp->engines[i],
           gsp->runlists[i], gsp->engines[i] == 0x1 ? "   <- compute channel's runlist" : "",
           gsp->engines[i] == want_et ? "   <- TINYNV_CE_GROUP" : "");
  }
  if (!gsp->ce_group || !gsp->ce_ctxshare || !gsp->ce_copy_q.entries) {
    printf("  the driver did not build the CE-group channel (group %#x, share %#x, ring %u entries): see the 'CE group "
           "probe' / 'CE channel probe' lines on stderr. Nothing to measure.\n", gsp->ce_group, gsp->ce_ctxshare,
           gsp->ce_copy_q.entries);
    return 1;
  }
  printf("CE group %#x on engine %#x runlist %u, channel %#x token %#x; graphics group on runlist %u\n", gsp->ce_group,
         gsp->ce_engine, gsp->ce_runlist, gsp->ce_copy_q.channel, gsp->ce_copy_q.token, gsp->channel_runlist);
  if (gsp->ce_runlist == gsp->channel_runlist)
    printf("  NOTE: this copy engine is on the SAME runlist as the compute channel. A second group there is scheduled by "
           "time-slicing against the graphics group, so expect SERIALIZED or PARTIAL, not CONCURRENT. On the 3060 the "
           "first copy engine with a runlist of its own is CE2, TINYNV_CE_GROUP=0xb.\n");
  if (EX->sync || EX->keepalive || EX->ring_async) {
    printf("  refused: this needs the default asynchronous submission with no keep-alive and no pipelined doorbell "
           "(sync %d, keepalive %d, ring_async %d) - otherwise the host itself orders the kernel and the copy\n",
           EX->sync, EX->keepalive, EX->ring_async);
    return 1;
  }

  // memory: the probe's own host page, one host source, two video-memory buffers
  if (tinynv_mm_alloc_buffer(&G->mm, PM_BYTES, 1, 1, 1, 0, 0, &pm) || !pm.dma.va) {
    printf("  cannot allocate the probe's host buffer: %s\n", tinynv_last_error());
    return 1;
  }
  memset(pm.dma.va, 0, PM_BYTES);   // the allocator's zero flag does nothing for host memory (exec.c says why)
  for (chunk = 64 * MIB; chunk >= 16 * MIB; chunk /= 2)
    if (!tinynv_mm_alloc_buffer(&G->mm, chunk, 1, 1, 1, 0, 0, &hsrc) && hsrc.dma.va) break;
  if (chunk < 16 * MIB) { printf("  no host buffer of 16 MiB or more could be mapped: %s\n", tinynv_last_error()); return 1; }
  if (tinynv_malloc(dev, chunk, &vA) != TINYNV_OK || tinynv_malloc(dev, chunk, &vB) != TINYNV_OK) {
    printf("  cannot allocate two %llu MiB video-memory buffers: %s\n", (unsigned long long)(chunk / MIB), tinynv_last_error());
    return 1;
  }
  printf("buffers: host source %llu MiB at gpu va %#llx, video memory %#llx and %#llx, probe page %#llx\n",
         (unsigned long long)(chunk / MIB), (unsigned long long)hsrc.va, (unsigned long long)vA, (unsigned long long)vB,
         (unsigned long long)pm.va);

  // P0: the CE-group channel's first traffic ever. Bind the copy class to subchannel 4, the same class new_queue
  // allocated on it (TINYNV_CE_CLASS when set, else the chip's own).
  uint32_t cls = G->dev.class_dma_copy;
  { const char *e = getenv("TINYNV_CE_CLASS"); if (e) cls = (uint32_t)strtoul(e, NULL, 0); }
  tinynv_cmdbuf_t c = cb_at(CB_SETUP, CB_SMALL);
  if (tinynv_cmd_copy_setup(&c, pm.va + ST_SETUP, 0, cls) || submit_cb(&gsp->ce_copy_q, CB_SETUP, &c)) {
    printf("  P0: building or submitting the copy setup failed: %s\n", tinynv_last_error());
    return 1;
  }
  if (wait_tags(ST_SETUP, 0, 1, 5.0, NULL, NULL)) { report_stall("P0, binding the copy class on the CE-group channel"); return 1; }
  printf("P0 the CE-group channel took its setup batch (class %#x on subchannel 4)\n", cls);
  // and one small copy through it, checked, before anything large
  // The P0 matrix: every cell is a 4 KB copy whose destination was pre-filled with the sentinel, so the readback says
  // which of "landed", "never written" (sentinel survives) and "written with zeros" (source read as 0) happened.
  // (a) is the control on the graphics copy_q; a failure there is the probe's own. A stall ends the probe at once; any
  // other failure lets the remaining cells run and then stops before anything large.
  {
    int bad = 0, rc;
    uint32_t tA = ++tagseq, tB;
    mark_src(tA);
#define CELL(...) do { rc = p0_cell(__VA_ARGS__); if (rc == -2) return 1; bad += rc != 0; } while (0)
    CELL("a", "control: h2d on graphics copy_q", &gsp->copy_q, vA, 0, hsrc.va, 1, tA);
    CELL("b", "h2d on the CE-group channel", &gsp->ce_copy_q, vA, 0, hsrc.va, 1, tA);
    // a pattern in video memory, put there by the runtime's own copy path (copy_q), then checked
    tB = ++tagseq;
    mark_src(tB);
    if (WITH_LOCK(tinynv_exec_copy(EX, vB, hsrc.va, VERIFY_BYTES)) || tinynv_stream_sync(S) != TINYNV_OK) {
      printf("P0d  could not place the d2d source through copy_q: %s\n", tinynv_last_error());
      return 1;
    }
    if (verify(vB, VERIFY_BYTES, tB, "P0d source in vB (written by graphics copy_q)")) bad++;
    else printf("P0d  source in vB holds its pattern (word 0 %#x), written through graphics copy_q\n", pattern(tB, 0));
    CELL("d", "d2d (vB -> vA) on the CE-group channel", &gsp->ce_copy_q, vA, 0, (uint64_t)vB, 0, tB);
    CELL("f", "h2h (host -> probe page) on the CE-group channel", &gsp->ce_copy_q, pm.va + PM_HOSTDST, 1, hsrc.va, 1, tB);
    CELL("g", "d2h (vB -> probe page) on the CE-group channel", &gsp->ce_copy_q, pm.va + PM_HOSTDST, 1, (uint64_t)vB, 0, tB);
#undef CELL
    if (bad) { printf("P0 FAILED in %d cell(s): stopping before any large copy\n", bad); return 1; }
  }
  printf("P0 every 4 KB copy on the CE-group channel landed and reads back right\n");

  // the kernel: load it (first launch uploads the module), then measure the SM clock with a short spin
  if (tinynv_module_load(dev, blob, (size_t)len, &mod) != TINYNV_OK || tinynv_get_kernel(mod, "tinynv_keepalive", &K) != TINYNV_OK ||
      tinynv_kernel_info(K, &KI) != TINYNV_OK) {
    printf("  cannot load tinynv_keepalive from %s: %s\n", cubin_path, tinynv_last_error());
    return 1;
  }
  if (KI.num_params != 2 || KI.params[0].size != 8 || KI.params[1].size != 8) {
    printf("  tinynv_keepalive takes %d parameters (%d, %d bytes); this probe passes a pointer and a u64\n", KI.num_params,
           KI.params[0].size, KI.params[1].size);
    return 1;
  }
  run_t w;
  if (one_run(P1, &w, 0, 1000000ull)) return 1;   // loads the module; not timed
  const uint64_t cal_cycles = 200000000ull;
  if (one_run(P1, &w, 0, cal_cycles)) return 1;
  double cyc_per_ns = (double)cal_cycles / (double)(w.k1 - w.k0);
  printf("calibration: %llu cycles took %.1f ms on the gpu clock (raw %llu..%llu): %.3f cycles/ns\n",
         (unsigned long long)cal_cycles, ms(w.k0, w.k1), (unsigned long long)w.k0, (unsigned long long)w.k1, cyc_per_ns);
  if (!(cyc_per_ns > 0.3 && cyc_per_ns < 3.5)) {
    printf("  that is not a believable SM clock; using 1.8 cycles/ns for the target instead\n");
    cyc_per_ns = 1.8;
  }
  cycles_for_kernel = (uint64_t)(kernel_ms * 1e6 * cyc_per_ns);

  // copy sizes, from measured rates: ~60% of the kernel each
  double target_ns = 0.6 * kernel_ms * 1e6;
  if (one_run(P2, &w, 4, 0)) return 1;
  double r_h2d = gbs(&w);
  if (one_run(P2B, &w, 16, 0)) return 1;
  double r_d2d = gbs(&w);
  #define CLAMP_REPS(x) ((x) < 2.0 ? 2u : (x) > (double)MAX_REPS ? MAX_REPS : (uint32_t)(x))
  reps_h2d = CLAMP_REPS(target_ns * r_h2d / (double)chunk);
  reps_d2d = CLAMP_REPS(target_ns * r_d2d / (double)chunk);
  printf("calibration: h2d %.2f GB/s -> %u x %llu MiB; d2d %.2f GB/s -> %u x %llu MiB; kernel %llu cycles\n", r_h2d,
         reps_h2d, (unsigned long long)(chunk / MIB), r_d2d, reps_d2d, (unsigned long long)(chunk / MIB),
         (unsigned long long)cycles_for_kernel);

  static run_t R[NPH][10];
  for (int p = 0; p < NPH; p++) {
    printf("%s %s\n", PH[p].tag, PH[p].what);
    for (int i = 0; i < runs; i++) {
      run_t *r = &R[p][i];
      if (one_run(p, r, 0, 0)) return 1;
      printf("  run %d: ", i);
      if (PH[p].kernel) printf("kernel %llu..%llu (%.1f ms) ", (unsigned long long)r->k0, (unsigned long long)r->k1, ms(r->k0, r->k1));
      if (PH[p].copy) printf("copy %llu..%llu (%.1f ms, %.2f GB/s) ", (unsigned long long)r->c0, (unsigned long long)r->c1,
                             ms(r->c0, r->c1), gbs(r));
      if (PH[p].kernel && PH[p].copy) {
        double in_ms, fr = overlap(r, &in_ms);
        uint64_t lo = r->k0 < r->c0 ? r->k0 : r->c0, hi = r->k1 > r->c1 ? r->k1 : r->c1;
        printf("copy starts %+.1f ms after the kernel; intersection %.1f ms = %.0f%% of the shorter; gpu wall %.1f ms; "
               "host wall %.1f ms -> %s", ((double)r->c0 - (double)r->k0) / 1e6, in_ms, 100 * fr, ms(lo, hi),
               r->wall * 1e3, verdict(fr));
      } else printf("host wall %.1f ms", r->wall * 1e3);
      printf("\n");
    }
  }

  // the summary, last, where the runner's tail shows it
  double mean_k = 0, mean_c[NPH] = {0}, mean_bw[NPH] = {0};
  for (int i = 0; i < runs; i++) mean_k += ms(R[P1][i].k0, R[P1][i].k1) / runs;
  for (int p = 0; p < NPH; p++)
    for (int i = 0; i < runs; i++) if (PH[p].copy) { mean_c[p] += ms(R[p][i].c0, R[p][i].c1) / runs; mean_bw[p] += gbs(&R[p][i]) / runs; }
  printf("SUMMARY engine %#x runlist %u (compute on runlist %u); kernel alone %.1f ms (target %.0f, %.3f cycles/ns)\n",
         gsp->ce_engine, gsp->ce_runlist, gsp->channel_runlist, mean_k, kernel_ms, cyc_per_ns);
  printf("  alone: h2d CE-group %.2f GB/s (%u x %llu MiB, %.1f ms); d2d CE-group %.2f GB/s (%u x, %.1f ms); h2d copy_q %.2f GB/s (%.1f ms)\n",
         mean_bw[P2], reps_h2d, (unsigned long long)(chunk / MIB), mean_c[P2], mean_bw[P2B], reps_d2d, mean_c[P2B],
         mean_bw[P2C], mean_c[P2C]);
  static const int OV[3] = {P3, P3B, P4}, ALONE[3] = {P2, P2B, P2C};
  const char *final[3];
  for (int o = 0; o < 3; o++) {
    int p = OV[o], conc = 0, ser = 0, disagree = 0;
    double sumf = 0;
    printf("  %s %-42s", PH[p].tag, PH[p].what);
    for (int i = 0; i < runs; i++) {
      double fr = overlap(&R[p][i], NULL);
      sumf += fr;
      conc += fr >= 0.5; ser += fr < 0.05;
      printf(" %s", verdict(fr));
      // the host's clock, independently: concurrent work cannot take as long as the two run back to back
      double serial_s = (mean_k + mean_c[ALONE[o]]) / 1e3, max_s = (mean_k > mean_c[ALONE[o]] ? mean_k : mean_c[ALONE[o]]) / 1e3;
      if ((fr >= 0.5 && R[p][i].wall > 0.9 * serial_s) || (fr < 0.05 && R[p][i].wall < max_s + 0.5 * (serial_s - max_s)))
        disagree++;
    }
    final[o] = conc == runs ? "CONCURRENT" : ser == runs ? "SERIALIZED" : "PARTIAL/MIXED";
    printf("; mean overlap %.0f%%, bw %.2f GB/s vs %.2f alone\n", 100 * sumf / runs, mean_bw[p], mean_bw[ALONE[o]]);
    if (disagree)
      printf("  !!! %s: the host wall clock disagrees with the gpu stamps in %d run(s) - read the per-run lines above\n",
             PH[p].tag, disagree);
  }
  if (strcmp(final[2], "SERIALIZED"))
    printf("  !!! CONTROL NOT SERIALIZED: a copy on the graphics-group copy_q overlapped the kernel (%s). The premise in "
           "gsp.c's comment - CE0 shares GR0's PBDMA so cannot overlap - does not hold on this card.\n", final[2]);
  printf("ANSWER: a copy on the engine-%#x group vs a running kernel: h2d %s, d2d %s; control (graphics copy_q) %s\n",
         gsp->ce_engine, final[0], final[1], final[2]);
  free(blob);
  return 0;   // every verdict is a result; 1 means the probe could not measure, 2 that it was not asked properly
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);   // the runner redirects stdout to a file and a card step can die mid-run
  int rc;
  if (!getenv("TINYNV_HW") || !getenv("TINYNV_SOCKET")) {
    printf("usage: TINYNV_HW=1 TINYNV_SOCKET=<socket> TINYNV_CE_GROUP=<engine type> %s [cubin] [kernel_ms=1000] [runs=3]\n",
           argv[0]);
    printf("  a card probe: run it through tools/nv_shim_step.sh <A|B> probe, which supplies both of the first two\n");
    rc = 2;
  } else if (!getenv("TINYNV_CE_GROUP") || !*getenv("TINYNV_CE_GROUP")) {
    printf("TINYNV_CE_GROUP is not set, so the driver builds no copy-engine group and there is nothing to measure.\n");
    printf("  On the RTX 3060 (GA106) use TINYNV_CE_GROUP=0xb: NV2080_ENGINE_TYPE_COPY2, the first copy engine on a\n");
    printf("  runlist of its own (runlist 1, PBDMA 5). TINYNV_CE_GROUP=0xa is NV2080_ENGINE_TYPE_COPY1, which on this card\n");
    printf("  shares GR0's runlist 0 and PBDMA 1 - a valid second run, but a time-sliced one.\n");
    rc = 2;
  } else {
    rc = probe(argc, argv);
    if (hung) printf("a wait timed out: the probe stopped submitting and leaves through the runtime's teardown\n");
  }
  // the card is put down by put_the_card_down (atexit), the same teardown libtinycudart gets
  return rc;
}

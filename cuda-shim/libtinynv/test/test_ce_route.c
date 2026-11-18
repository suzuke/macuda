// TINYNV_CE_ROUTE's ordering, checked without a card: what every batch acquires when bulk host-to-device chunks go to a
// copy-engine channel of their own, and what happens when that channel cannot be brought up.
//
// As test_upload_order: the REAL exec.c runs against recording stand-ins for the command encoders and the submit. Each
// submit closes a batch and names its queue: compute, copy (the graphics group's copy queue) or ce (the dedicated
// channel). The semaphore memory is mirrored so that waits complete; ORDER is judged from the record, never from it.
//
// Every arm is also run through one invariant, the one a hang on the card would come from: NO ACQUIRE NAMES A VALUE
// WHOSE RELEASING BATCH HAS NOT BEEN SUBMITTED BEFORE IT (submitted_only below).
//
// Arms (stage A, conservative):
//   chain then routed   launch A pending, a 2-chunk upload, launch B -> A's batch goes out BEFORE the first ce batch
//                       (the pending chain is kicked, not waited for); each ce batch acquires compute >= A; the second
//                       chunk does not re-acquire it; B's batch acquires ce >= the last chunk
//   held then routed    a held inline upload, then a routed upload -> the inline bytes go out in a compute batch of their
//                       own, before the ce batch, which acquires that batch's value
//   copy-queue work     a device-to-device copy, a routed upload, another copy -> the ce batch acquires the first copy's
//                       value on the copy slot; the second copy's batch acquires the ce value
//   staging reuse       three chunks through two staging buffers -> the third chunk's memcpy waits (on the CPU) for the
//                       ce value of the first, the chunk that read the same buffer
//   download after      a routed upload, then a download through staging buffer 0 -> the CPU waits for the ce value of
//                       the chunk that read buffer 0 before the download's copy writes it
//   idle covers ce      tinynv_exec_idle waits on the ce timeline too
//   routing off         the same guest calls with ce_route 0 -> no ce batch, no acquire on the ce slot
//   fallback            TINYNV_CE_ROUTE unset / group not ready / TINYNV_SYNC / allocation refused / self-check bytes
//                       wrong / self-check never completes -> ce_route stays 0, nothing half-set; set up whole -> 1
#include "exec.h"
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// ---- the record ----
enum { EV_WAIT, EV_INLINE, EV_LAUNCH, EV_BARRIER, EV_RELEASE, EV_COPY, EV_SUBMIT, EV_SETOBJ, EV_CPUWAIT_CE };
enum { Q_COMPUTE, Q_COPY, Q_CE };
static const char *QN[] = {"compute", "copy", "ce"};
static struct { int kind, batch; uint64_t a, b; } ev[1024];
static int nev, batch;
static uint64_t sem_base;
static void rec(int kind, uint64_t a, uint64_t b) { if (nev < 1024) { ev[nev].kind = kind; ev[nev].batch = batch; ev[nev].a = a; ev[nev].b = b; nev++; } }
static int take(tinynv_cmdbuf_t *c, uint32_t dwords) {
  if (c->n + dwords > c->cap) return tinynv_fail("the batch was sized for %u dwords and needs %u", c->cap, c->n + dwords);
  c->n += dwords;
  return 0;
}
static uint64_t slot_of(uint64_t addr) { return (addr - sem_base) / 128; }

// ---- the CE bring-up's host buffer (tinynv_mm_alloc_buffer stand-in) ----
static uint8_t ce_host[1 << 16] __attribute__((aligned(4096)));
static const uint64_t CE_VA = 0x40000000ull;
static int alloc_fail, copy_corrupt, ce_hold;

// ---- recording stand-ins (dword counts as in submit.c) ----
int tinynv_cmd_wait(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { rec(EV_WAIT, slot_of(addr), value); return take(c, 6); }
int tinynv_cmd_inline_upload(tinynv_cmdbuf_t *c, uint64_t dst, const void *src, uint32_t n) { (void)src; rec(EV_INLINE, dst, n); return take(c, TINYNV_INLINE_DWORDS(n)); }
int tinynv_cmd_launch(tinynv_cmdbuf_t *c, uint64_t qmd_addr) { rec(EV_LAUNCH, qmd_addr, 0); return take(c, 4); }
int tinynv_cmd_memory_barrier(tinynv_cmdbuf_t *c) { rec(EV_BARRIER, 0, 0); return take(c, 2); }
int tinynv_cmd_release(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { rec(EV_RELEASE, slot_of(addr), value); return take(c, 8); }
int tinynv_cmd_release_clocked(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { return tinynv_cmd_release(c, addr, value); }
int tinynv_cmd_copy_release(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { rec(EV_RELEASE, slot_of(addr), value); return take(c, 6); }
int tinynv_cmd_copy(tinynv_cmdbuf_t *c, uint64_t dst, uint64_t src, uint64_t bytes) {
  rec(EV_COPY, dst, bytes);
  // a host-to-host copy inside the CE buffer (the bring-up's self-check) is carried out, unless told to corrupt it
  if (!copy_corrupt && dst >= CE_VA && src >= CE_VA && dst + bytes <= CE_VA + sizeof ce_host && src + bytes <= CE_VA + sizeof ce_host)
    memcpy(ce_host + (dst - CE_VA), ce_host + (src - CE_VA), bytes);
  return take(c, 9u * (uint32_t)((bytes + (1ull << 31) - 1) >> 31));
}
int tinynv_cmd_set_object(tinynv_cmdbuf_t *c, uint32_t subch, uint32_t cls) { rec(EV_SETOBJ, subch, cls); return take(c, 2); }
int tinynv_cmd_copy_setup(tinynv_cmdbuf_t *c, uint64_t sem_addr, uint64_t sem_value, uint32_t copy_class) {
  if (tinynv_cmd_wait(c, sem_addr, sem_value)) return -1;
  if (tinynv_cmd_set_object(c, 4, copy_class)) return -1;
  return tinynv_cmd_release(c, sem_addr, sem_value + 1);
}
int tinynv_cmd_timestamp(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { (void)addr; (void)value; return take(c, 6); }
int tinynv_cmd_copy_timestamp(tinynv_cmdbuf_t *c, uint64_t addr, uint64_t value) { (void)addr; (void)value; return take(c, 6); }

static tinynv_exec_t *cur;
static uint8_t sem_host[4096] __attribute__((aligned(128)));
static struct { uint64_t va, value; } placed[64];
static int nplaced;
static uint64_t ce_held;   // ce_hold: the highest ce release submitted and not yet mirrored
static void mirror(int slot, uint64_t v) { uint64_t o; memcpy(&o, sem_host + slot * 128, 8); if (v > o) memcpy(sem_host + slot * 128, &v, 8); }
int tinynv_submit(tinynv_gpu_t *g, tinynv_queue_t *q, uint64_t cmdbuf_va, uint32_t dwords) {
  (void)cmdbuf_va; (void)dwords;
  int qi = q == &g->gsp.ce_copy_q ? Q_CE : q == &g->gsp.copy_q ? Q_COPY : Q_COMPUTE;
  for (int i = 0; i < nev && cur && cur->flush_n > 1; i++)   // a chain's linked descriptors, as test_upload_order
    if (ev[i].batch == batch && ev[i].kind == EV_LAUNCH && ev[i].b == 0 && ev[i].a == cur->chain[0].va) {
      int links = cur->flush_n - 1;
      if (nev + links > 1024) break;
      memmove(&ev[i + 1 + links], &ev[i + 1], (size_t)(nev - i - 1) * sizeof ev[0]);
      for (int k = 0; k < links; k++) { ev[i + 1 + k].kind = EV_LAUNCH; ev[i + 1 + k].batch = batch; ev[i + 1 + k].a = cur->chain[1 + k].va; ev[i + 1 + k].b = 1; }
      nev += links;
      break;
    }
  for (int i = 0; i < nev; i++) {
    if (ev[i].batch != batch) continue;
    if (ev[i].kind == EV_RELEASE && ev[i].a < 3) {
      if (ev[i].a == 2 && ce_hold) { if (ev[i].b > ce_held) ce_held = ev[i].b; }
      else mirror((int)ev[i].a, ev[i].b);
    }
    if (ev[i].kind == EV_LAUNCH) for (int k = 0; k < nplaced; k++) if (placed[k].va == ev[i].a) mirror(0, placed[k].value);
  }
  rec(EV_SUBMIT, (uint64_t)qi, 0); batch++; return 0;
}
// Ringing is where a held ce release lands (ce_hold == 1): a CPU wait on the ce slot rings first, so the record shows
// the wait (EV_CPUWAIT_CE) at the point it happened. ce_hold == 2 never lands it: a channel that never completes.
int tinynv_submit_ring(tinynv_gpu_t *g) {
  (void)g;
  if (ce_hold == 1 && ce_held) { rec(EV_CPUWAIT_CE, ce_held, 0); mirror(2, ce_held); ce_held = 0; }
  return 0;
}
int tinynv_submit_stage(tinynv_gpu_t *g, tinynv_queue_t *q, uint64_t cmdbuf_va, uint32_t dwords) { return tinynv_submit(g, q, cmdbuf_va, dwords); }
uint64_t tinynv_arena_place(const tinynv_exec_region_t *rg, uint64_t bytes, uint64_t align, int *wrap) {
  (void)align; *wrap = 0; uint64_t off = rg->next; ((tinynv_exec_region_t *)rg)->next += (bytes + 255) & ~255ull; return off;
}
uint64_t tinynv_arena_enter(tinynv_exec_region_t *rg, uint64_t off, uint64_t bytes) { (void)rg; (void)off; (void)bytes; return 0; }
void tinynv_arena_take(tinynv_exec_region_t *rg, uint64_t off, uint64_t bytes) { (void)rg; (void)off; (void)bytes; }
void tinynv_arena_submitted(tinynv_exec_region_t *rg, uint64_t value) { (void)rg; (void)value; }
void tinynv_arena_mark(tinynv_exec_region_t *rg, uint64_t lo, uint64_t hi, uint64_t value) { (void)rg; (void)lo; (void)hi; (void)value; }
int tinynv_arena_rewind(tinynv_exec_region_t *rg) { (void)rg; return 0; }
int tinynv_qmd_link_check(const tinynv_qmd_t *q, int releases, uint64_t want_payload, uint64_t next_va, const char **why) {
  (void)q; (void)releases; (void)want_payload; (void)next_va; (void)why; return 0;
}
int tinynv_qmd_membar(tinynv_qmd_t *q, int membar) { (void)q; (void)membar; return 0; }
int tinynv_qmd_release_clear(tinynv_qmd_t *q) { (void)q; return 0; }
int tinynv_gsp_poll(tinynv_gpu_t *g) { (void)g; return 0; }
void tinynv_note_host_write(const void *dst, size_t n, const char *what) { (void)dst; (void)n; (void)what; }
void tinynv_note_host_region(const void *base, size_t n, const char *what) { (void)base; (void)n; (void)what; }
// the three FAKE_EXEC_CE_ROUTE takes out of fake_exec_unreachable.c
int tinynv_mm_alloc_buffer(tinynv_mm_t *mm, uint64_t size, int host, int cpu_access, int uncached, int force_devmem,
                           int zero, tinynv_vmap_t *out) {
  (void)mm; (void)host; (void)cpu_access; (void)uncached; (void)force_devmem; (void)zero;
  if (alloc_fail || size > sizeof ce_host) return tinynv_fail("the allocator refused (test)");
  memset(out, 0, sizeof *out);
  out->va = CE_VA; out->size = size; out->dma.va = ce_host;
  return 0;
}
void tinynv_queue_state(tinynv_gpu_t *g, tinynv_queue_t *q, uint32_t *get, uint32_t *put, uint64_t *published) {
  (void)g; (void)q; *get = *put = 0; *published = 0;
}
void tinynv_vmap_free(tinynv_mm_t *mm, tinynv_vmap_t *m) { (void)mm; memset(m, 0, sizeof *m); }

// ---- an exec with no card behind it ----
#define STAGE_HALF (16u << 20)   // exec.c's STAGE_BYTES: an upload is cut into chunks of this, whatever the buffer size
static uint8_t *stage_buf;
static uint8_t cmd_shadow[1 << 20], qmd_host[TINYNV_EXEC_CHAIN_MAX][TINYNV_QMD_BYTES];
static tinynv_gpu_t *gpu;
static tinynv_exec_t *fresh(int route) {
  static tinynv_exec_t ex;
  memset(&ex, 0, sizeof ex);
  if (!gpu) gpu = calloc(1, sizeof *gpu);
  if (!stage_buf) stage_buf = malloc(2ull * STAGE_HALF);
  memset(&gpu->gsp.ce_copy_q, 0, sizeof gpu->gsp.ce_copy_q);
  gpu->gsp.ce_copy_q.entries = 64; gpu->gsp.ce_ready = 1; gpu->gsp.ce_engine = 0xb;
  gpu->gsp.ce_runlist = 1; gpu->gsp.channel_runlist = 0;
  ex.g = gpu;
  ex.sem.va = sem_base = 0x30000000ull;
  memset(sem_host, 0, sizeof sem_host); ex.sem.dma.view.ptr = (volatile uint32_t *)sem_host; ex.sem.dma.view.size = sizeof sem_host;
  ex.region[1].shadow = cmd_shadow; ex.region[1].mem.va = 0x10000000ull; ex.region[1].size = sizeof cmd_shadow;
  ex.stage[0].dma.va = stage_buf; ex.stage[0].va = 0x20000000ull; ex.stage[0].size = STAGE_HALF;
  ex.stage[1].dma.va = stage_buf + STAGE_HALF; ex.stage[1].va = 0x20000000ull + STAGE_HALF; ex.stage[1].size = STAGE_HALF;
  ex.inline_upload = 1; ex.inline_pend_max = 16; ex.inline_pend_bytes_max = 8192;
  ex.inline_max = 4096;
  if (route) {   // as tinynv_exec_ce_route_init leaves it: the bring-up used ce values 1 (bind) and 2 (self-check)
    ex.ce_route = 1;
    ex.ce_cmd.va = CE_VA; ex.ce_cmd.dma.va = ce_host; ex.ce_cmd.size = sizeof ce_host;
    ex.ce_reserved = 2; ex.ce_cmd_val[1] = 1; ex.ce_cmd_val[2] = 2; ex.ce_acq[0] = ex.ce_acq[1] = 2;
    uint64_t two = 2; memcpy(sem_host + 256, &two, 8);
  }
  nev = 0; batch = 0; nplaced = 0; cur = &ex; ce_hold = 0; ce_held = 0; copy_corrupt = 0; alloc_fail = 0;
  return &ex;
}
static void launch(tinynv_exec_t *ex, uint64_t qmd_va) {
  memset(&ex->chain[ex->nchain], 0, sizeof ex->chain[ex->nchain]);
  ex->chain[ex->nchain].va = qmd_va;
  ex->chain[ex->nchain].host = qmd_host[ex->nchain];
  ex->nchain++;
  ex->reserved++;
  placed[nplaced].va = qmd_va; placed[nplaced++].value = ex->reserved;
}
static void show(const char *arm) {
  printf("  %s:", arm);
  for (int i = 0; i < nev; i++) {
    if (ev[i].kind == EV_SUBMIT) printf(" |%s", QN[ev[i].a]);
    else if (ev[i].kind == EV_WAIT) printf(" wait(%s>=%llu)", QN[ev[i].a], (unsigned long long)ev[i].b);
    else if (ev[i].kind == EV_RELEASE) printf(" rel(%s=%llu)", QN[ev[i].a], (unsigned long long)ev[i].b);
    else if (ev[i].kind == EV_BARRIER) continue;
    else if (ev[i].kind == EV_LAUNCH) printf(" %s(%#llx)", ev[i].b ? "link" : "launch", (unsigned long long)ev[i].a);
    else if (ev[i].kind == EV_INLINE) printf(" inline(%#llx)", (unsigned long long)ev[i].a);
    else if (ev[i].kind == EV_COPY) printf(" copy(%#llx)", (unsigned long long)ev[i].a);
    else if (ev[i].kind == EV_SETOBJ) printf(" setobj(%#llx)", (unsigned long long)ev[i].b);
    else if (ev[i].kind == EV_CPUWAIT_CE) printf(" CPU-WAIT(ce>=%llu)", (unsigned long long)ev[i].a);
  }
  printf("\n");
}
// the queue a batch went to
static int bq(int b) { for (int i = 0; i < nev; i++) if (ev[i].kind == EV_SUBMIT && ev[i].batch == b) return (int)ev[i].a; return -1; }
// the n-th (0-based) batch on queue q, or -1
static int nth_batch(int q, int n) { for (int i = 0; i < nev; i++) if (ev[i].kind == EV_SUBMIT && (int)ev[i].a == q && n-- == 0) return ev[i].batch; return -1; }
// the highest wait on `slot` inside batch b, 0 if none
static uint64_t waits_in(int b, int slot) { uint64_t m = 0; for (int i = 0; i < nev; i++) if (ev[i].batch == b && ev[i].kind == EV_WAIT && (int)ev[i].a == slot && ev[i].b > m) m = ev[i].b; return m; }
static uint64_t rel_in(int b, int slot) { for (int i = 0; i < nev; i++) if (ev[i].batch == b && ev[i].kind == EV_RELEASE && (int)ev[i].a == slot) return ev[i].b; return 0; }
static int first(int kind, uint64_t a) { for (int i = 0; i < nev; i++) if (ev[i].kind == kind && ev[i].a == a) return i; return -1; }
static int count_q(int q) { int n = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == EV_SUBMIT && (int)ev[i].a == q) n++; return n; }

// THE invariant: every acquire names a value some EARLIER-SUBMITTED batch releases (a launched descriptor releases the
// value the test placed for it). `base` is what the slots held before the record started.
static void submitted_only(const char *arm, const uint64_t base[3]) {
  uint64_t done[3] = {base[0], base[1], base[2]}, mine[3] = {0, 0, 0};
  for (int i = 0; i < nev; i++) {
    if (ev[i].kind == EV_WAIT && ev[i].a < 3)
      CHECK(ev[i].b <= done[ev[i].a], "%s: batch %d acquires %s >= %llu, and only %llu has been released by a submitted batch",
            arm, ev[i].batch, QN[ev[i].a], (unsigned long long)ev[i].b, (unsigned long long)done[ev[i].a]);
    if (ev[i].kind == EV_RELEASE && ev[i].a < 3 && ev[i].b > mine[ev[i].a]) mine[ev[i].a] = ev[i].b;
    if (ev[i].kind == EV_LAUNCH) for (int k = 0; k < nplaced; k++) if (placed[k].va == ev[i].a && placed[k].value > mine[0]) mine[0] = placed[k].value;
    if (ev[i].kind == EV_SUBMIT) for (int s = 0; s < 3; s++) { if (mine[s] > done[s]) done[s] = mine[s]; mine[s] = 0; }
  }
}

int main(void) {
  const uint64_t A = 0xA000, B = 0xB000, U = 0x5000000, D = 0x6000000;
  uint8_t *data = calloc(3, STAGE_HALF);
  const uint64_t base_route[3] = {0, 0, 2}, base_off[3] = {0, 0, 0};

  { // chain then routed: the pending chain is kicked first, the ce batches wait for it, B waits for the ce batches
    tinynv_exec_t *ex = fresh(1);
    launch(ex, A);
    CHECK(!tinynv_exec_upload(ex, U, data, 2 * STAGE_HALF), "chain/routed: upload: %s", tinynv_last_error());
    launch(ex, B);
    CHECK(!tinynv_exec_flush(ex), "chain/routed: flush: %s", tinynv_last_error());
    show("chain then routed");
    submitted_only("chain/routed", base_route);
    int a = first(EV_LAUNCH, A), ce0 = nth_batch(Q_CE, 0), ce1 = nth_batch(Q_CE, 1), b = first(EV_LAUNCH, B);
    CHECK(count_q(Q_CE) == 2, "chain/routed: %d ce batches for a 2-chunk upload, want 2", count_q(Q_CE));
    CHECK(count_q(Q_COPY) == 0 || first(EV_COPY, U) < 0 || bq(ev[first(EV_COPY, U)].batch) == Q_CE,
          "chain/routed: a chunk went to the graphics copy queue");
    CHECK(a >= 0 && ce0 >= 0 && ev[a].batch < ce0, "chain/routed: launch A was not submitted before the first routed chunk");
    CHECK(ce0 >= 0 && waits_in(ce0, Q_COMPUTE) >= 1, "chain/routed: the first chunk does not acquire compute >= 1 (launch A)");
    CHECK(ce1 >= 0 && waits_in(ce1, Q_COMPUTE) == 0, "chain/routed: the second chunk re-acquires a compute value already acquired");
    CHECK(ce0 >= 0 && rel_in(ce0, Q_CE) == 3 && ce1 >= 0 && rel_in(ce1, Q_CE) == 4,
          "chain/routed: the ce batches release %llu, %llu on the ce slot, want 3, 4 (their own counter)",
          (unsigned long long)rel_in(ce0, Q_CE), (unsigned long long)rel_in(ce1, Q_CE));
    CHECK(b >= 0 && waits_in(ev[b].batch, Q_CE) == 4, "chain/routed: launch B's batch acquires ce >= %llu, want 4 (the last chunk)",
          (unsigned long long)(b >= 0 ? waits_in(ev[b].batch, Q_CE) : 0));
    CHECK(ex->stage_pend_ce[0] == 3 && ex->stage_pend_ce[1] == 4 && !ex->stage_pend[0] && !ex->stage_pend[1],
          "chain/routed: staging buffers record ce %llu/%llu and copy %llu/%llu, want ce 3/4 and nothing on the copy queue",
          (unsigned long long)ex->stage_pend_ce[0], (unsigned long long)ex->stage_pend_ce[1],
          (unsigned long long)ex->stage_pend[0], (unsigned long long)ex->stage_pend[1]);
  }
  { // held then routed: the held bytes are flushed in a compute batch before the routed copy, which waits for it
    tinynv_exec_t *ex = fresh(1);
    CHECK(!tinynv_exec_upload(ex, 0x7000, data, 16), "held/routed: small upload: %s", tinynv_last_error());
    CHECK(ex->inline_pend_n == 1, "held/routed: the small upload was not held (%u held)", ex->inline_pend_n);
    CHECK(!tinynv_exec_upload(ex, U, data, STAGE_HALF), "held/routed: upload: %s", tinynv_last_error());
    show("held then routed");
    submitted_only("held/routed", base_route);
    int u = first(EV_INLINE, 0x7000), ce0 = nth_batch(Q_CE, 0);
    CHECK(u >= 0 && bq(ev[u].batch) == Q_COMPUTE, "held/routed: the held bytes did not go out in a compute batch");
    CHECK(u >= 0 && ce0 > ev[u].batch, "held/routed: the held bytes were not submitted before the routed chunk");
    uint64_t rel = u >= 0 ? rel_in(ev[u].batch, Q_COMPUTE) : 0;
    CHECK(rel && ce0 >= 0 && waits_in(ce0, Q_COMPUTE) >= rel,
          "held/routed: the routed chunk acquires compute >= %llu, not the held batch's %llu",
          (unsigned long long)(ce0 >= 0 ? waits_in(ce0, Q_COMPUTE) : 0), (unsigned long long)rel);
    CHECK(ex->inline_pend_n == 0, "held/routed: bytes still held after the routed upload");
  }
  { // copy-queue work on both sides of a routed chunk
    tinynv_exec_t *ex = fresh(1);
    CHECK(!tinynv_exec_copy(ex, D, D + 0x100000, 4096), "copyq: first copy: %s", tinynv_last_error());
    CHECK(!tinynv_exec_upload(ex, U, data, STAGE_HALF), "copyq: upload: %s", tinynv_last_error());
    CHECK(!tinynv_exec_copy(ex, D + 0x200000, U, 4096), "copyq: second copy: %s", tinynv_last_error());
    show("copy-queue work");
    submitted_only("copyq", base_route);
    int c0 = nth_batch(Q_COPY, 0), ce0 = nth_batch(Q_CE, 0), c1 = nth_batch(Q_COPY, 1);
    uint64_t r0 = c0 >= 0 ? rel_in(c0, Q_COPY) : 0;
    CHECK(r0 && ce0 >= 0 && waits_in(ce0, Q_COPY) >= r0, "copyq: the routed chunk does not acquire the earlier copy (copy >= %llu)",
          (unsigned long long)r0);
    CHECK(c1 >= 0 && waits_in(c1, Q_CE) == 3, "copyq: the copy after the routed chunk acquires ce >= %llu, want 3",
          (unsigned long long)(c1 >= 0 ? waits_in(c1, Q_CE) : 0));
  }
  { // staging reuse: the third chunk refills buffer 0 only after the first chunk's ce value (CPU wait)
    tinynv_exec_t *ex = fresh(1);
    ce_hold = 1;
    CHECK(!tinynv_exec_upload(ex, U, data, 3 * STAGE_HALF), "reuse: upload: %s", tinynv_last_error());
    show("staging reuse");
    submitted_only("reuse", base_route);
    int ce2 = nth_batch(Q_CE, 2), w = -1;
    for (int i = 0; i < nev; i++) if (ev[i].kind == EV_CPUWAIT_CE && ev[i].a >= 3) { w = i; break; }
    CHECK(w >= 0 && ce2 >= 0 && ev[w].batch <= ce2, "reuse: the third chunk was built before the CPU waited for the first (ce >= 3)");
  }
  { // download after a routed chunk read staging buffer 0
    tinynv_exec_t *ex = fresh(1);
    ce_hold = 1;
    CHECK(!tinynv_exec_upload(ex, U, data, STAGE_HALF), "download: upload: %s", tinynv_last_error());
    static uint8_t out[256];
    CHECK(!tinynv_exec_download(ex, out, U, sizeof out), "download: %s", tinynv_last_error());
    show("download after routed");
    submitted_only("download", base_route);
    int w = -1, dc = -1;
    for (int i = 0; i < nev; i++) if (ev[i].kind == EV_CPUWAIT_CE && ev[i].a >= 3 && w < 0) w = i;
    for (int i = 0; i < nev; i++) if (ev[i].kind == EV_COPY && ev[i].a == ex->stage[0].va) dc = i;
    CHECK(w >= 0 && dc >= 0 && w < dc, "download: the copy into staging buffer 0 was built before the CPU saw the routed chunk (ce >= 3) finish");
    CHECK(ex->stage_pend_ce[0] == 0, "download: staging buffer 0 still records a ce reader");
  }
  { // idle covers the ce timeline
    tinynv_exec_t *ex = fresh(1);
    ce_hold = 1;
    CHECK(!tinynv_exec_upload(ex, U, data, STAGE_HALF), "idle: upload: %s", tinynv_last_error());
    // ce_high is what the ce slot was last READ as: nothing has looked at it yet, and idle must look until it holds 3
    // (any ring lands the held release, so the record alone cannot tell idle's wait from someone else's ring)
    CHECK(ex->ce_high < 3, "idle: the ce slot was already read as %llu before the idle", (unsigned long long)ex->ce_high);
    CHECK(!tinynv_exec_idle(ex), "idle: %s", tinynv_last_error());
    CHECK(ex->ce_high >= 3, "idle: tinynv_exec_idle returned without reading the ce channel at 3 (read %llu)", (unsigned long long)ex->ce_high);
  }
  { // routing off: identical calls, nothing on the ce channel or its slot
    tinynv_exec_t *ex = fresh(0);
    launch(ex, A);
    CHECK(!tinynv_exec_upload(ex, 0x7000, data, 16), "off: small: %s", tinynv_last_error());
    CHECK(!tinynv_exec_upload(ex, U, data, 2 * STAGE_HALF), "off: upload: %s", tinynv_last_error());
    CHECK(!tinynv_exec_copy(ex, D, U, 4096), "off: copy: %s", tinynv_last_error());
    launch(ex, B);
    CHECK(!tinynv_exec_idle(ex), "off: idle: %s", tinynv_last_error());
    show("routing off");
    submitted_only("off", base_off);
    int on_ce = 0; for (int i = 0; i < nev; i++) if ((ev[i].kind == EV_WAIT || ev[i].kind == EV_RELEASE) && ev[i].a == 2) on_ce++;
    CHECK(count_q(Q_CE) == 0 && on_ce == 0, "off: %d ce batches and %d ce-slot methods with routing off", count_q(Q_CE), on_ce);
    CHECK(count_q(Q_COPY) >= 3, "off: the chunks did not go to the graphics copy queue");
  }

  // ---- the bring-up and its fallbacks ----
  struct { const char *name; const char *env; int ready, sync, alloc_fail, corrupt, hold, want; } fb[] = {
    {"unset", NULL, 1, 0, 0, 0, 0, 0},
    {"group not ready", "1", 0, 0, 0, 0, 0, 0},
    {"TINYNV_SYNC", "1", 1, 1, 0, 0, 0, 0},
    {"allocation refused", "1", 1, 0, 1, 0, 0, 0},
    {"self-check bytes wrong", "1", 1, 0, 0, 1, 0, 0},
    {"self-check never completes", "1", 1, 0, 0, 0, 2, 0},
    {"whole", "1", 1, 0, 0, 0, 0, 1},
  };
  for (unsigned k = 0; k < sizeof fb / sizeof fb[0]; k++) {
    tinynv_exec_t *ex = fresh(0);
    gpu->gsp.ce_ready = fb[k].ready;
    ex->sync = fb[k].sync;
    alloc_fail = fb[k].alloc_fail; copy_corrupt = fb[k].corrupt; ce_hold = fb[k].hold;
    if (fb[k].env) setenv("TINYNV_CE_ROUTE", fb[k].env, 1); else unsetenv("TINYNV_CE_ROUTE");
    printf("  fallback/%s:\n", fb[k].name);
    fflush(stdout);
    int rc = tinynv_exec_ce_route_init(ex);
    CHECK(rc == 0, "fallback/%s: the bring-up returned %d; a refusal must fall back, not fail the boot", fb[k].name, rc);
    CHECK(ex->ce_route == fb[k].want, "fallback/%s: ce_route is %d, want %d", fb[k].name, ex->ce_route, fb[k].want);
    if (!fb[k].want) {
      int cleared = !ex->ce_reserved && !ex->ce_acq_q[0] && !ex->ce_acq_q[1];
      for (int s = 0; s < TINYNV_CE_CMD_SLOTS; s++) cleared &= !ex->ce_cmd_val[s];
      CHECK(cleared, "fallback/%s: the ce counter or its slots were left set (ce_reserved %llu)", fb[k].name, (unsigned long long)ex->ce_reserved);
      // and a routing-off upload afterwards goes where it always went
      int before = count_q(Q_CE);
      CHECK(!tinynv_exec_upload(ex, U, data, STAGE_HALF), "fallback/%s: upload: %s", fb[k].name, tinynv_last_error());
      CHECK(count_q(Q_CE) == before, "fallback/%s: an upload after the fallback went to the ce channel", fb[k].name);
    } else {
      show("bring-up");
      CHECK(ex->ce_acq[0] == 2 && ex->ce_acq[1] == 2, "whole: the graphics queues would still acquire the bring-up's ce values");
      CHECK(ex->ce_reserved == 2, "whole: the bring-up used ce values up to %llu, want 2 (bind, self-check)", (unsigned long long)ex->ce_reserved);
      int b0 = nth_batch(Q_CE, 0), b1 = nth_batch(Q_CE, 1);
      int so = -1; for (int i = 0; i < nev; i++) if (ev[i].kind == EV_SETOBJ && ev[i].batch == b0) so = i;
      CHECK(so >= 0 && ev[so].a == 4, "whole: the first ce batch does not bind the copy class on subchannel 4");
      CHECK(b1 >= 0 && first(EV_COPY, CE_VA + (uint64_t)TINYNV_CE_CMD_SLOTS * 512 + 4096) >= 0 && rel_in(b1, Q_CE) == 2,
            "whole: the self-check is not a copy released as ce value 2");
      submitted_only("whole", base_off);
    }
  }
  unsetenv("TINYNV_CE_ROUTE");
  printf(fails ? "%d of %d checks failed\n" : "ce route: all %d checks passed\n", fails ? fails : checks, checks);
  return fails ? 1 : 0;
}

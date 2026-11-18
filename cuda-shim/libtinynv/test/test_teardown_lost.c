// test_teardown_lost.c - M7 of the 2026-11-18 DART panic analysis, offline: the teardown of a gpu latched lost
// (src/lost.c) must not drain it, tear its exec down or unload GSP-RM - each of those waits on a card that cannot
// answer - and must do the one thing left: for a lost server, the emergency bus-master clear (waited for, bounded);
// with the connection alive, gpu_close, whose quiesce clears bus mastering before anything is let go.
// Mechanics as test_teardown_skip.c: tinynv.c is compiled in, every driver layer is a stub, no card.
#include "../src/tinynv.c"

static int checks, tfails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); tfails++; } } while (0)

// The order of what the teardown did, one letter each: I idle (the drain), F exec_fini, C gpu_close, R the rescue
// (with how long it was told to wait), Q a quiesce outside gpu_close.
static char g_did[64];
static double g_rescue_wait;
static int g_idle_latches, g_dead;
static void did(char c) { size_t n = strlen(g_did); if (n + 1 < sizeof g_did) { g_did[n] = c; g_did[n + 1] = 0; } }
int tinynv_exec_idle(tinynv_exec_t *ex) { (void)ex; did('I'); if (g_idle_latches) g_dev.gpu.lost = TINYNV_LOST_SERVER; return g_idle_latches ? -1 : 0; }
void tinynv_exec_fini(tinynv_exec_t *ex) { (void)ex; did('F'); }
void tinynv_gpu_close(tinynv_gpu_t *g) { (void)g; did('C'); }
void tinynv_dev_quiesce(tinynv_dev_t *d) { (void)d; did('Q'); }
static void fake_rescue(tinynv_pci_t *p, double wait_s) { (void)p; did('R'); g_rescue_wait = wait_s; }
static int fake_dead(tinynv_pci_t *p) { (void)p; return g_dead; }
static void __attribute__((noreturn)) unreachable(const char *f) { printf("  FAIL: teardown reached %s\n", f); _exit(97); }
#define STOP(name, ret, ...) ret name(__VA_ARGS__) { unreachable(#name); }
STOP(tinynv_exec_init, int, tinynv_gpu_t *g, tinynv_exec_t *e)
STOP(tinynv_gpu_open, int, tinynv_gpu_t *g, tinynv_pci_t *p)
STOP(tinynv_gpu_init_sw, int, tinynv_gpu_t *g)
STOP(tinynv_gpu_init_hw, int, tinynv_gpu_t *g)
// every other undefined symbol tinynv.c carries belongs to paths the child never takes (launch, upload, cubin,
// sensors, gsp setup); a stop for each keeps the link honest and the first wrong turn loud
STOP(tinynv_exec_copy, int, tinynv_exec_t *e, uint64_t d, uint64_t s, uint64_t n)
STOP(tinynv_exec_download, int, tinynv_exec_t *e, void *d, uint64_t s, size_t n)
STOP(tinynv_exec_upload, int, tinynv_exec_t *e, uint64_t d, const void *s, size_t n)
STOP(tinynv_exec_zero, int, tinynv_exec_t *e, uint64_t va, uint64_t n)
STOP(tinynv_exec_flush, int, tinynv_exec_t *e)
STOP(tinynv_exec_wait, int, tinynv_exec_t *e, uint64_t v, double s)
STOP(tinynv_exec_reached, int, tinynv_exec_t *e, uint64_t v)
STOP(tinynv_exec_wait_ce, int, tinynv_exec_t *e, uint64_t v, double s)
STOP(tinynv_exec_reached_ce, int, tinynv_exec_t *e, uint64_t v)
STOP(tinynv_exec_order_record, void, tinynv_exec_t *e, uint64_t q[2], uint64_t *ce, uint32_t *epoch)
STOP(tinynv_exec_order_wait, void, tinynv_exec_t *e, const uint64_t q[2], uint64_t ce, uint32_t epoch)
STOP(tinynv_exec_needs_wait, int, const tinynv_exec_t *e)
STOP(tinynv_exec_run, int, tinynv_exec_t *e, tinynv_exec_module_t *m, const tinynv_kernel_desc_t *k, const uint32_t grid[3], const uint32_t block[3], uint32_t sm, const void *p, size_t n)
STOP(tinynv_exec_load, int, tinynv_exec_t *e, const tinynv_cubin_t *c, const char *kn, tinynv_exec_module_t *m)
STOP(tinynv_exec_unload, void, tinynv_mm_t *mm, tinynv_exec_module_t *m)
STOP(tinynv_exec_set_internal, void, tinynv_exec_t *e, int on)
STOP(tinynv_exec_stage_va, uint64_t, const tinynv_exec_t *e)
STOP(tinynv_exec_stage_host, void *, const tinynv_exec_t *e)
STOP(tinynv_exec_stage_bytes, size_t, void)
STOP(tinynv_exec_stage_claim, int, tinynv_exec_t *e)
STOP(tinynv_exec_keepalive_arm, int, tinynv_exec_t *e, uint64_t *flag_va)
STOP(tinynv_exec_keepalive_release, void, tinynv_exec_t *e, int by_wait)
STOP(tinynv_exec_graph_begin, int, tinynv_exec_t *e)
STOP(tinynv_exec_graph_end, int, tinynv_exec_t *e, tinynv_graph_rec_t **out)
STOP(tinynv_exec_graph_launch, int, tinynv_exec_t *e, tinynv_graph_rec_t *r)
STOP(tinynv_exec_graph_free, void, tinynv_exec_t *e, tinynv_graph_rec_t *r)
STOP(tinynv_cubin_parse, int, const void *b, size_t n, tinynv_cubin_t *c)
STOP(tinynv_cubin_free, void, tinynv_cubin_t *c)
STOP(tinynv_cubin_kernel, const tinynv_kernel_desc_t *, const tinynv_cubin_t *c, const char *n)
STOP(tinynv_cubin_global, int, const tinynv_cubin_t *c, const char *n, uint64_t *off, uint64_t *size)
STOP(tinynv_cubin_image, int, const tinynv_cubin_t *c, const char *kernel, int with_bytes, tinynv_image_t *im)
STOP(tinynv_image_free, void, tinynv_image_t *im)
STOP(tinynv_kernel_thread_limit, unsigned, const tinynv_kernel_desc_t *d)
STOP(tinynv_mm_alloc_buffer, int, tinynv_mm_t *mm, uint64_t size, int host, int cpu_access, int uncached, int force_devmem, int zero, tinynv_vmap_t *out)
STOP(tinynv_mm_check_fw_carveout, int, const tinynv_mm_t *mm, uint64_t lo, uint64_t hi, char *l, size_t n)
STOP(tinynv_vmap_free, void, tinynv_mm_t *mm, tinynv_vmap_t *v)
STOP(tinynv_va_tag_set, void, const char *tag)
STOP(tinynv_pci_open_tinygpu, int, const char *s, tinynv_pci_t *out)
STOP(tinynv_rd32, uint32_t, tinynv_dev_t *d, uint64_t off)
STOP(tinynv_gsp_init_objects, int, tinynv_gpu_t *g)
STOP(tinynv_gsp_init_channel, int, tinynv_gpu_t *g)
STOP(tinynv_gsp_init_gr_context, int, tinynv_gpu_t *g)
STOP(tinynv_gsp_init_queues, int, tinynv_gpu_t *g)
STOP(tinynv_gsp_open_client, int, tinynv_gpu_t *g)
STOP(tinynv_gsp_free_heap, int, tinynv_gpu_t *g, uint64_t *out)
STOP(tinynv_gsp_sensors_init, int, tinynv_gpu_t *g, uint64_t poll_mask, uint32_t freq_ms)
STOP(tinynv_gsp_sensors_read, int, tinynv_gpu_t *g, tinynv_rusd_t *out)
STOP(tinynv_tlsf_used, uint64_t, const tinynv_tlsf_t *a)
STOP(tinynv_tlsf_size, uint64_t, const tinynv_tlsf_t *a)



static tinynv_pci_t g_fake_pci;
static void arm(const char *name, int lost, int dead, int idle_latches, const char *want, double want_wait) {
  memset(&g_dev, 0, sizeof g_dev);
  g_did[0] = 0; g_rescue_wait = 0; g_dead = dead; g_idle_latches = idle_latches;
  g_fake_pci = (tinynv_pci_t){.live = 1, .transport_dead = fake_dead, .rescue = fake_rescue};
  g_dev.gpu.dev.pci = &g_fake_pci;
  g_dev.gpu.lost = lost;
  g_dev.booted = 1; g_dev.exec_ready = 1;
  printf("  arm: %s\n", name);
  put_the_card_down();
  CHECK(!strcmp(g_did, want), "%s: the teardown did \"%s\", want \"%s\" (I drain, F exec_fini, C gpu_close, R rescue)", name, g_did, want);
  CHECK(g_rescue_wait == want_wait, "%s: the rescue was waited for %.1f s, want %.1f", name, g_rescue_wait, want_wait);
  CHECK(g_dev.booted == 0 && g_dev.torn_down == 1, "%s: the re-entry guard flags must be set", name);
  CHECK(!g_dev.exec_ready || !strcmp(want, "IFC"), "%s: exec_ready left set", name);
}

int main(void) {
  printf("teardown of a lost gpu (M7)\n");
  // the ordinary teardown is unchanged
  arm("not lost", 0, 0, 0, "IFC", 0);
  // a lost server: no drain, no exec teardown, no gpu_close (so no unload, no quiesce through a dead connection);
  // the emergency clear, waited for with a bound. (The real rescue refuses anything but a latched loss.)
  arm("server lost, latched", TINYNV_LOST_SERVER, 1, 0, "R", 6.0);
  // the transport saw it first and nothing else had: the teardown latches it itself, at no cost - and the latch
  // starts the clear (R, not waited for), which the teardown then waits for (R, 6 s)
  arm("server gone, seen only by the transport", 0, 1, 0, "RR", 6.0);
  // the transport alive: no drain, no exec teardown; gpu_close, whose unload refuses a lost gpu and whose quiesce
  // clears bus mastering before the frees (test_lost checks that half against the real gpu.c and backend)
  arm("window lost", TINYNV_LOST_WINDOW, 0, 0, "C", 0);
  arm("card off the bus", TINYNV_LOST_CARD, 0, 0, "C", 0);
  // the drain is what found the loss: what follows it is the lost teardown, not exec_fini and an unload
  arm("server lost during the drain", 0, 0, 1, "IR", 6.0);
  printf("teardown lost: %d checks run, %d failed\n", checks, tfails);
  return tfails ? 1 : 0;
}

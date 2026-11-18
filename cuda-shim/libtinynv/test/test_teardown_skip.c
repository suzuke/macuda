// test_teardown_skip.c - TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL, offline: when the exit-path drain fails, an opted-in
// teardown must NOT unload the GSP / clear mastering (tinynv_gpu_close) nor tear down the exec (tinynv_exec_fini).
//
// Arms (each calls the REAL static put_the_card_down from ../src/tinynv.c, with the drain stubbed to succeed or fail):
//   unset + drain fails  -> gpu_close AND exec_fini still run (today's behaviour, byte-identical default)
//   =0    + drain fails  -> same
//   =1    + drain fails  -> neither runs, and the warning is printed  (FAILS on a tree without the opt-in)
//   =1    + drain ok     -> both run (the opt-in only changes the failure path)
// Mechanics as test_teardown_term.c: tinynv.c is compiled in, every driver layer is a stub, no card.
#include "../src/tinynv.c"

static int checks, tfails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); tfails++; } } while (0)

static int g_idle_ret, g_idle_calls, g_close_calls, g_fini_calls;
int tinynv_exec_idle(tinynv_exec_t *ex) { (void)ex; g_idle_calls++; return g_idle_ret; }
void tinynv_exec_fini(tinynv_exec_t *ex) { (void)ex; g_fini_calls++; }
void tinynv_gpu_close(tinynv_gpu_t *g) { (void)g; g_close_calls++; }
void tinynv_dev_quiesce(tinynv_dev_t *d) { (void)d; }
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


static void arm(const char *name, const char *env, int idle_ret, int want_close, int want_fini) {
  if (env) setenv("TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL", env, 1); else unsetenv("TINYNV_SKIP_UNLOAD_ON_DRAIN_FAIL");
  g_idle_ret = idle_ret; g_idle_calls = g_close_calls = g_fini_calls = 0;
  memset(&g_dev, 0, sizeof g_dev);
  g_dev.booted = 1; g_dev.exec_ready = 1;
  printf("  arm: %s\n", name);
  put_the_card_down();
  CHECK(g_idle_calls == 1, "%s: the drain must be attempted once (%d)", name, g_idle_calls);
  CHECK(g_close_calls == want_close, "%s: tinynv_gpu_close calls %d, want %d", name, g_close_calls, want_close);
  CHECK(g_fini_calls == want_fini, "%s: tinynv_exec_fini calls %d, want %d", name, g_fini_calls, want_fini);
  CHECK(g_dev.booted == 0 && g_dev.torn_down == 1, "%s: the re-entry guard flags must be set", name);
}

int main(void) {
  printf("teardown skip-unload-on-drain-fail\n");
  arm("env unset, drain fails (default: unload anyway)", NULL, -1, 1, 1);
  arm("env=0, drain fails (default: unload anyway)", "0", -1, 1, 1);
  arm("env=1, drain fails (skip unload)", "1", -1, 0, 0);
  arm("env=1, drain ok (unload as usual)", "1", 0, 1, 1);
  printf("teardown skip-unload: %d of %d checks passed%s\n", checks - tfails, checks, tfails ? "" : " - all");
  return tfails;
}

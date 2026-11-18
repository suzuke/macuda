// ce_hazard - the two open hardware hazards of TINYNV_CE_ROUTE, reproduced on purpose, bounded, one per run. A card
// probe, not built by `all`; every mode needs TINYNV_CE_ROUTE=1 to mean anything (it says so and exits 2 otherwise).
//
//   H1  the ~2 s semaphore-acquire timeout every channel is allocated with (NVOS04_FLAGS_CHANNEL_PBDMA_ACQUIRE_TIMEOUT:
//       "if a host semaphore acquire does not complete in about 2 sec, it will time out and trigger a RC error"). A
//       routed copy that must follow queued compute would sit on that acquire for the whole backlog. libtinynv now has
//       the HOST wait for the backlog first (ce_submit); TINYNV_CE_NO_HOSTWAIT=1 puts the acquire back on the channel.
//   H2  teardown with work outstanding on the copy channel, which has never been exercised.
//
// Modes (ms is the compute backlog queued ahead of the copy, calibrated with a blocking read, at most 4000):
//   acquire <ms>     gemms on stream S for ~ms, then a 32 MiB cudaMemcpyAsync on S (routed, must follow them), then
//                    cudaStreamSynchronize(S) and a check of the copied bytes and the gemm result. Exit 0 = PASS.
//   exit-async       a 256 MiB cudaMemcpyAsync on S and return from main with no synchronise: the atexit teardown must
//                    idle both timelines (an upload returns with at most two 16 MiB chunks outstanding).
//   exit-stuck <ms>  as acquire, but return right after issuing the copy, with no synchronise. Only with
//                    TINYNV_CE_NO_HOSTWAIT=1 is anything stuck on the copy channel at exit.
//
// Run order, control first, each followed by a boot check (`... A probe cuda-shim/build/shim-sm_86/copy_bw 64 2`):
//   1  TINYNV_CE_ROUTE=1 ce_hazard acquire 1000                       expect PASS, nothing new
//   2  TINYNV_CE_ROUTE=1 ce_hazard acquire 3000                       expect PASS: the host waits ~3 s, no RC error
//   3  TINYNV_CE_ROUTE=1 ce_hazard exit-async                         expect exit 0 and a clean next boot
//   4  TINYNV_CE_ROUTE=1 TINYNV_CE_NO_HOSTWAIT=1 ce_hazard acquire 1000   the channel parks ~1 s; expect PASS
//   5  TINYNV_CE_ROUTE=1 TINYNV_CE_NO_HOSTWAIT=1 ce_hazard acquire 3000   THE H1 HAZARD: expect an RC error / mmu or
//                    channel error from gsp-rm after ~2 s, the copy never completing, the synchronise failing after 30 s
//   6  TINYNV_CE_ROUTE=1 TINYNV_CE_NO_HOSTWAIT=1 ce_hazard exit-stuck 3000  H1 then H2: exit with the channel in error
// WORST CASE, 1-4: none expected beyond an ordinary failed run. 5 and 6: MAY WEDGE THE CARD - the teardown then unloads
// gsp-rm with a channel in RC error, which is untried; recovery = enclosure power cycle (needed and sufficient after the
// 2026-11-18 copy_overlap wedge). STOP RULE: stop at the first run that exits non-zero, or whose boot check fails; do
// not run 6 if 5 wedged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include "tinynv.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { printf("FAIL %s: %s (after %.2f s)\n", #x, cudaGetErrorString(e_), now() - t_start); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t e_ = (x); if (e_ != CUBLAS_STATUS_SUCCESS) { printf("FAIL %s: cublas status %d\n", #x, (int)e_); exit(1); } } while (0)

static double t_start;
static const int n = 2048;
static float *dA, *dB, *dC;
static cublasHandle_t h;

static void block_read(void) { float v; CK(cudaMemcpy(&v, dC, sizeof v, cudaMemcpyDeviceToHost)); }
static void gemms(cudaStream_t s, int reps) {
  const float one = 1.0f, zero = 0.0f;
  CB(cublasSetStream_v2(h, s));
  for (int i = 0; i < reps; i++) CB(cublasSgemm_v2(h, CUBLAS_OP_N, CUBLAS_OP_N, n, n, n, &one, dA, n, dB, n, &zero, dC, n));
}

int main(int argc, char **argv) {
  t_start = now();
  const char *mode = argc > 1 ? argv[1] : "";
  int ms = argc > 2 ? atoi(argv[2]) : 1000;
  int m_acq = !strcmp(mode, "acquire"), m_async = !strcmp(mode, "exit-async"), m_stuck = !strcmp(mode, "exit-stuck");
  if (!m_acq && !m_async && !m_stuck) { fprintf(stderr, "usage: ce_hazard acquire <ms> | exit-async | exit-stuck <ms>\n"); return 2; }
  if (ms < 100 || ms > 4000) { fprintf(stderr, "ms must be 100..4000\n"); return 2; }
  int ndev = 0; CK(cudaGetDeviceCount(&ndev)); if (!ndev) { fprintf(stderr, "no device\n"); return 2; }
  const size_t mat = (size_t)n * n * 4, cbytes = (m_async ? 256u : 32u) << 20;
  float *hm = malloc(mat); unsigned char *hc = malloc(cbytes), *back = malloc(cbytes);
  if (!hm || !hc || !back) { fprintf(stderr, "malloc failed\n"); return 2; }
  for (size_t i = 0; i < mat / 4; i++) hm[i] = 1.0f;
  for (size_t i = 0; i < cbytes; i++) hc[i] = (unsigned char)(i * 2654435761u >> 13);
  unsigned char *dcopy;
  CK(cudaMalloc((void **)&dA, mat)); CK(cudaMalloc((void **)&dB, mat)); CK(cudaMalloc((void **)&dC, mat));
  CK(cudaMalloc((void **)&dcopy, cbytes));
  CK(cudaMemcpy(dA, hm, mat, cudaMemcpyHostToDevice)); CK(cudaMemcpy(dB, hm, mat, cudaMemcpyHostToDevice));
  cudaStream_t s; CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
  CB(cublasCreate_v2(&h));
  gemms(s, 1); block_read();   // warm, and boots the card
  tinynv_device_t d = NULL; unsigned et = 0, rl = 0;
  if (tinynv_device_get(&d, 0) != TINYNV_OK || !tinynv_device_copy_route(d, &et, &rl)) {
    printf("copy route is OFF: set TINYNV_CE_ROUTE=1 (or it was refused at boot - see the libtinynv lines). Nothing to probe.\n");
    return 2;
  }
  const char *nohw = getenv("TINYNV_CE_NO_HOSTWAIT");
  printf("copy route: ON, engine 0x%x runlist %u; host wait before a dependent copy: %s\n", et, rl,
         nohw && *nohw && *nohw != '0' ? "OFF (TINYNV_CE_NO_HOSTWAIT=1, the hazard)" : "on (the fix)");
  if (m_async) {
    CK(cudaMemcpyAsync(dcopy, hc, cbytes, cudaMemcpyHostToDevice, s));
    printf("exit-async: a %zu MiB copy issued, returning from main without a synchronise (%.2f s in)\n", cbytes >> 20, now() - t_start);
    return 0;
  }
  double t0 = now(); gemms(s, 4); block_read(); double one = (now() - t0) / 4;
  if (one < 0.5e-3 || one > 0.25) { printf("FAIL one gemm measured %.3f ms, outside 0.5..250: refusing\n", one * 1e3); return 1; }
  int reps = (int)(ms * 1e-3 / one + 0.5); if (reps > 1024) reps = 1024; if (reps < 1) reps = 1;
  printf("backlog: %d gemms of %.2f ms = %.0f ms of compute ahead of a %zu MiB copy that must follow it\n", reps, one * 1e3,
         reps * one * 1e3, cbytes >> 20);
  t0 = now();
  gemms(s, reps);
  CK(cudaMemcpyAsync(dcopy, hc, cbytes, cudaMemcpyHostToDevice, s));
  double t_issue = now() - t0;
  printf("the copy call returned after %.0f ms (the host waits for the backlog first unless TINYNV_CE_NO_HOSTWAIT)\n", t_issue * 1e3);
  if (m_stuck) { printf("exit-stuck: returning from main without a synchronise\n"); return 0; }
  CK(cudaStreamSynchronize(s));
  double t_all = now() - t0;
  CK(cudaMemcpy(back, dcopy, cbytes, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(hm, dC, mat, cudaMemcpyDeviceToHost));
  int bad = memcmp(back, hc, cbytes) != 0;
  size_t wrong = 0; for (size_t i = 0; i < mat / 4; i++) wrong += hm[i] != (float)n;
  printf("copy and backlog done in %.0f ms; copied bytes %s; gemm result %s\n", t_all * 1e3, bad ? "WRONG" : "ok",
         wrong ? "WRONG" : "ok");
  printf(bad || wrong ? "RESULT: FAIL\n" : "RESULT: PASS\n");
  return bad || wrong ? 1 : 0;
}

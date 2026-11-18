// copy_overlap - does a big host-to-device copy on one stream run underneath a kernel on another (TINYNV_CE_ROUTE),
// and do the ordering guarantees CUDA gives still hold when it does. A card probe, not built by `all`:
//   QUIESCE=1 sh tools/nv_shim_step.sh A probe cuda-shim/build/shim-<arch>/copy_overlap [copy MiB=512] [gemm n=2048]
//   QUIESCE=1 TINYNV_CE_ROUTE=1 sh tools/nv_shim_step.sh A probe cuda-shim/build/shim-<arch>/copy_overlap
// Timing: R cuBLAS SGEMMs on stream A alone, one cudaMemcpyAsync of the copy size on stream B alone, then both issued
// together (the gemms first, so they are on the card while the copy streams in). r = (both - max) / min: r < 0.3
// OVERLAP, r > 0.7 SERIAL, else PARTIAL. Routing off is expected to say SERIAL - that is the control.
//
// HOW MUCH WORK IT MAY QUEUE, AND WHY THAT IS BOUNDED (the 2026-11-18 run queued ~18,500 gemms, ~100 s of work, timed
// out after 30 s and unloaded the firmware with the engines still busy: two mmu faults, and the card stayed wedged until
// the enclosure was power-cycled. TEARING DOWN UNDER LOAD WEDGES THE CARD - so this probe must never exit with much
// queued, on success, on error, or on a timeout):
//  - one gemm is timed with a BLOCKING read (a 4-byte device-to-host cudaMemcpy, which always idles the device), never
//    with a stream synchronise; and refused, exit 1 before anything big is queued, if it is implausibly fast (under
//    0.5 ms at n=2048, scaled by (n/2048)^3) or slower than 0.25 s;
//  - R is at most 256 and at most 0.5 s of gemms; every batch of kernels is followed by a blocking read;
//  - the whole run's planned GPU work (4 R gemm batches, 5 big copies, a few single gemms) must be under 3 s, else exit 1
//    after the two measuring copies (a smaller copy size is the fix);
//  - a copy never leaves more than two 16 MiB chunks queued: an upload returns only once its last chunk is submitted.
// So at any moment at most ~0.5 s of gemms plus two chunks are outstanding, which also keeps every semaphore acquire
// well inside the ~2 s channel acquire timeout (NVOS04_FLAGS_CHANNEL_PBDMA_ACQUIRE_TIMEOUT).
// Correctness, each checked with sentinel values through a gemm (C = n * a when A is filled with a and B with 1), the
// matrix the gemm reads sitting in the LAST 16 MiB of a big copy so a gemm ordered wrongly would read the old value:
//   raw      copy (a=3) then gemm on the same stream                      -> C = 3n
//   war      gemms on a 16 MiB matrix of 1s, then a copy of 2s into that matrix alone, same stream -> C = n. One
//            chunk, so a copy that overtook the gemms would land while they still run and C would read 2n
//   lwar     the same, the gemms on A and the copy a synchronous cudaMemcpy on the LEGACY stream, which must wait for
//            every stream's earlier work -> C = n
//   ssync    copy (a=5) on B, cudaStreamSynchronize(B), gemm on C          -> C = 5n
//   dsync    copy (a=6) on B, cudaDeviceSynchronize, gemm on C             -> C = 6n
//   event    copy (a=7) on B, record on B, cudaStreamWaitEvent(A), gemm on A -> C = 7n
// Exit 0 only when every check passes; the verdict itself never fails the run.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include "tinynv.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s: %s\n", #x, cudaGetErrorString(e_)); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t e_ = (x); if (e_ != CUBLAS_STATUS_SUCCESS) { fprintf(stderr, "%s: cublas status %d\n", #x, (int)e_); exit(1); } } while (0)

static int n;              // gemm dimension
static size_t big, mat;    // copy bytes, one matrix's bytes
static unsigned char *hbig;
static float *hmat, *dones, *dC;
static cublasHandle_t h;

// Wait for EVERYTHING on the device, by a read that cannot return early: a download idles the device before it copies.
static void block_read(void) { float v; CK(cudaMemcpy(&v, dC, sizeof v, cudaMemcpyDeviceToHost)); }
static void drain(cudaStream_t s) { CK(cudaStreamSynchronize(s)); block_read(); }

static void gemms(cudaStream_t s, const float *A, int reps) {
  const float one = 1.0f, zero = 0.0f;
  CB(cublasSetStream_v2(h, s));
  for (int i = 0; i < reps; i++) CB(cublasSgemm_v2(h, CUBLAS_OP_N, CUBLAS_OP_N, n, n, n, &one, A, n, dones, n, &zero, dC, n));
}
// the big copy, its last matrix's worth filled with `a`
static void copy_big(unsigned char *dst, float a, cudaStream_t s) {
  float *tail = (float *)(hbig + big - mat);
  for (size_t i = 0; i < mat / 4; i++) tail[i] = a;
  CK(cudaMemcpyAsync(dst, hbig, big, cudaMemcpyHostToDevice, s));
}
// C after the gemm, against n * a, read on stream s
static int check(const char *name, cudaStream_t s, float a) {
  CK(cudaMemcpyAsync(hmat, dC, mat, cudaMemcpyDeviceToHost, s));
  CK(cudaStreamSynchronize(s));
  float want = (float)n * a;
  size_t bad = 0, first = 0;
  for (size_t i = 0; i < mat / 4; i++) if (hmat[i] != want) { if (!bad++) first = i; }
  if (bad) printf("FAIL %-6s C[%zu] = %g, want %g (%zu of %zu wrong)\n", name, first, hmat[first], want, bad, mat / 4);
  else printf("ok   %-6s C = %g everywhere\n", name, want);
  return bad != 0;
}

int main(int argc, char **argv) {
  size_t mib = argc > 1 ? (size_t)atoi(argv[1]) : 512;
  n = argc > 2 ? atoi(argv[2]) : 2048;
  if (mib < 32 || mib > 1024 || n < 256 || n > 4096) { fprintf(stderr, "copy MiB must be 32..1024 and n 256..4096\n"); return 1; }
  big = mib << 20; mat = (size_t)n * n * 4;
  if (big < 2 * mat) { fprintf(stderr, "the copy (%zu MiB) must be at least two matrices (%zu bytes each)\n", mib, mat); return 1; }
  int ndev = 0; CK(cudaGetDeviceCount(&ndev)); if (!ndev) { fprintf(stderr, "no device\n"); return 1; }
  hbig = calloc(1, big); hmat = malloc(mat);
  float *hones = malloc(mat), *htwos = malloc(mat);
  if (!hbig || !hmat || !hones || !htwos) { fprintf(stderr, "malloc failed\n"); return 1; }
  for (size_t i = 0; i < mat / 4; i++) { hones[i] = 1.0f; htwos[i] = 2.0f; }
  unsigned char *dbig, *dbig2; float *dA, *dW;
  CK(cudaMalloc((void **)&dbig, big)); CK(cudaMalloc((void **)&dbig2, big));
  CK(cudaMalloc((void **)&dA, mat)); CK(cudaMalloc((void **)&dones, mat)); CK(cudaMalloc((void **)&dC, mat));
  CK(cudaMalloc((void **)&dW, mat));
  CK(cudaMemcpy(dA, hones, mat, cudaMemcpyHostToDevice)); CK(cudaMemcpy(dones, hones, mat, cudaMemcpyHostToDevice));
  cudaStream_t sa, sb, sc;
  CK(cudaStreamCreateWithFlags(&sa, cudaStreamNonBlocking)); CK(cudaStreamCreateWithFlags(&sb, cudaStreamNonBlocking));
  CK(cudaStreamCreateWithFlags(&sc, cudaStreamNonBlocking));
  CB(cublasCreate_v2(&h));
  cudaEvent_t ev; CK(cudaEventCreateWithFlags(&ev, cudaEventDisableTiming));

  // ---- timing ----
  gemms(sa, dA, 1); block_read();                                         // warm: module load, first touch
  double t0 = now(); gemms(sa, dA, 4); block_read(); double one = (now() - t0) / 4;
  double fast = 0.5e-3 * ((double)n / 2048) * ((double)n / 2048) * ((double)n / 2048);
  if (one < fast || one > 0.25) {
    fprintf(stderr, "copy_overlap: one %d^3 gemm measured %.3f ms, outside %.3f..250 ms: the timing cannot be trusted, "
                    "and planning from it could queue unbounded work. Refusing.\n", n, one * 1e3, fast * 1e3);
    return 1;
  }
  copy_big(dbig, 1.0f, sb); drain(sb);
  // asked once the device is up (the warm-up above booted it): routing is decided at boot and may have been refused
  tinynv_device_t tdev = NULL; unsigned et = 0, rl = 0; int routed = 0;
  if (tinynv_device_get(&tdev, 0) == TINYNV_OK) routed = tinynv_device_copy_route(tdev, &et, &rl);
  t0 = now(); copy_big(dbig, 1.0f, sb); drain(sb); double tc = now() - t0;
  int reps = (int)(tc / one + 0.5);
  if (reps > 256) reps = 256;
  if (reps * one > 0.5) reps = (int)(0.5 / one);
  if (reps < 1) reps = 1;
  double plan = 4 * reps * one + 5 * tc + 6 * one;
  if (plan > 3.0) {
    fprintf(stderr, "copy_overlap: the run would queue %.2f s of GPU work (copy %.0f ms, %d gemms of %.2f ms), over the 3 s "
                    "ceiling. Refusing; pass a smaller copy size.\n", plan, tc * 1e3, reps, one * 1e3);
    return 1;
  }
  printf("plan: %d gemms of %.2f ms per batch (%.0f ms), copy %.0f ms; %.2f s of GPU work in all\n", reps, one * 1e3,
         reps * one * 1e3, tc * 1e3, plan);
  t0 = now(); gemms(sa, dA, reps); drain(sa); double tg = now() - t0;
  t0 = now(); gemms(sa, dA, reps); copy_big(dbig, 1.0f, sb); CK(cudaStreamSynchronize(sb)); drain(sa);
  double tb = now() - t0;
  int fails = check("timing", sa, 1.0f);   // the gemms of the overlapped run, not disturbed by the copy

  // ---- correctness ----
  // raw: copy a=3 into the matrix the gemm then reads, same stream
  float *tailA = (float *)(dbig + big - mat), *tailB = (float *)(dbig2 + big - mat);
  copy_big(dbig, 3.0f, sb); gemms(sb, tailA, 1); fails += check("raw", sb, 3.0f);
  // war: W holds 1 (set and synchronised); the gemms read it, then a one-chunk copy of 2s into W on the same stream
  CK(cudaMemcpy(dW, hones, mat, cudaMemcpyHostToDevice));
  gemms(sb, dW, reps); CK(cudaMemcpyAsync(dW, htwos, mat, cudaMemcpyHostToDevice, sb)); fails += check("war", sb, 1.0f);
  block_read();
  // lwar: as war, but the gemms on A and the overwrite on the legacy stream (cudaMemcpy), ordered after all streams
  CK(cudaMemcpy(dW, hones, mat, cudaMemcpyHostToDevice));
  gemms(sa, dW, reps); CK(cudaMemcpy(dW, htwos, mat, cudaMemcpyHostToDevice)); fails += check("lwar", sa, 1.0f);
  block_read();
  // ssync / dsync: a copy on B, then a CPU synchronisation, then a gemm on a third stream that knows nothing of B
  copy_big(dbig2, 5.0f, sb); CK(cudaStreamSynchronize(sb)); gemms(sc, tailB, 1); fails += check("ssync", sc, 5.0f);
  copy_big(dbig2, 6.0f, sb); CK(cudaDeviceSynchronize()); gemms(sc, tailB, 1); fails += check("dsync", sc, 6.0f);
  // event: a copy on B, recorded; A waits for it on the GPU and its gemm reads the copied matrix
  copy_big(dbig2, 7.0f, sb); CK(cudaEventRecord(ev, sb)); CK(cudaStreamWaitEvent(sa, ev, 0));
  gemms(sa, tailB, 1); fails += check("event", sa, 7.0f);

  double r = (tb - (tg > tc ? tg : tc)) / (tg < tc ? tg : tc);
  printf("copy %zu MiB on its own stream: %.1f ms (%.2f GB/s); %d gemms of %d^3 on another: %.1f ms\n", mib, tc * 1e3,
         big / tc / 1e9, reps, n, tg * 1e3);
  printf("both at once: %.1f ms; serial would be %.1f, perfect overlap %.1f; r = %.2f\n", tb * 1e3, (tg + tc) * 1e3,
         (tg > tc ? tg : tc) * 1e3, r);
  if (routed) printf("copy route: ON, copy engine type 0x%x (CE%u) on runlist %u\n", et, et >= 0x9 ? et - 0x9 : et, rl);
  else printf("copy route: OFF (every copy on the graphics copy queue)\n");
  printf("VERDICT: %s\n", r < 0.3 ? "OVERLAP" : r > 0.7 ? "SERIAL" : "PARTIAL");
  printf(fails ? "RESULT: FAIL, %d correctness checks failed\n" : "RESULT: PASS, all 7 correctness checks passed\n", fails);
  return fails ? 1 : 0;
}

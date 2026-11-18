// copy_bw - host<->device copy bandwidth through the shim, the number that bounds anything streaming weights or
// experts from host memory (expert offload, the CUDA-prefill half of tools/disagg-router.py, partial -ngl).
// For each transfer size: pinned (cudaMallocHost) and pageable (malloc) host memory, host->device and device->host,
// timed as `reps` back-to-back cudaMemcpyAsync on one stream and one synchronisation. A probe: run it through
//   QUIESCE=1 sh tools/nv_shim_step.sh A probe cuda-shim/build/shim-<arch>/copy_bw [max MiB=256] [reps=8]
// It checks the bytes too (the last device->host copy against what was uploaded), so a fast wrong answer fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cuda_runtime.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { fprintf(stderr, "%s: %s\n", #x, cudaGetErrorString(e_)); exit(1); } } while (0)

static double run(void *dst, const void *src, size_t n, enum cudaMemcpyKind k, int reps, cudaStream_t s) {
  CK(cudaMemcpyAsync(dst, src, n, k, s)); CK(cudaStreamSynchronize(s));   // warm: first-touch mappings, staging
  double t0 = now();
  for (int i = 0; i < reps; i++) CK(cudaMemcpyAsync(dst, src, n, k, s));
  CK(cudaStreamSynchronize(s));
  return now() - t0;
}

int main(int argc, char **argv) {
  size_t max_mib = argc > 1 ? (size_t)atoi(argv[1]) : 256; int reps = argc > 2 ? atoi(argv[2]) : 8;
  size_t max = max_mib << 20;
  int dev = 0; CK(cudaGetDeviceCount(&dev)); if (!dev) { fprintf(stderr, "no device\n"); return 1; }
  unsigned char *pin, *pin_back, *pg = malloc(max), *pg_back = malloc(max), *d;
  CK(cudaMallocHost((void **)&pin, max)); CK(cudaMallocHost((void **)&pin_back, max)); CK(cudaMalloc((void **)&d, max));
  if (!pg || !pg_back) { fprintf(stderr, "malloc failed\n"); return 1; }
  for (size_t i = 0; i < max; i++) pin[i] = pg[i] = (unsigned char)(i * 2654435761u >> 13);
  cudaStream_t s; CK(cudaStreamCreateWithFlags(&s, 0));
  printf("copy_bw: %d copies per point, one sync; GB/s = 1e9 bytes/s; us = per copy\n", reps);
  printf("%10s  %18s  %18s  %18s  %18s\n", "size", "pinned H2D", "pinned D2H", "pageable H2D", "pageable D2H");
  int bad = 0;
  for (size_t n = 64 << 10; n <= max; n *= 4) {
    double t[4];
    t[0] = run(d, pin, n, cudaMemcpyHostToDevice, reps, s);
    t[1] = run(pin_back, d, n, cudaMemcpyDeviceToHost, reps, s);
    bad |= memcmp(pin_back, pin, n) != 0;
    t[2] = run(d, pg, n, cudaMemcpyHostToDevice, reps, s);
    t[3] = run(pg_back, d, n, cudaMemcpyDeviceToHost, reps, s);
    bad |= memcmp(pg_back, pg, n) != 0;
    char sz[16]; snprintf(sz, sizeof sz, n >= (1 << 20) ? "%zu MiB" : "%zu KiB", n >= (1 << 20) ? n >> 20 : n >> 10);
    printf("%10s", sz);
    for (int i = 0; i < 4; i++) printf("  %7.2f GB/s %6.0fus", (double)n * reps / t[i] / 1e9, t[i] / reps * 1e6);
    printf("\n");
    if (n * 4 > max && n != max) n = max / 4;   // end exactly on max
  }
  printf(bad ? "copy_bw: DATA MISMATCH\n" : "copy_bw: data verified\n");
  return bad;
}

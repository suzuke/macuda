// Pointer-array batches under a stream capture (2026-11-19), card-free on the null device; test/batched_capture_check.sh
// reads what this prints, what graph.c prints at the end of the capture, and the launch trace.
// cublasGemmBatchedEx / cublasSgemmBatched / cublasStrsmBatched take arrays of matrix pointers in DEVICE memory. ggml fills
// those arrays with a kernel issued just before the call (k_compute_batched_ptrs, solve_tri's get_batch_pointers), and
// under a capture that kernel is only recorded, so at the time of the call the arrays hold whatever was there before.
// This fills them with POISON before capturing, as that unrun kernel would leave them, and requires that each call be
// recorded as ONE node of a *_ptrs kernel that holds the ARRAYS' addresses (read on the device at replay), not N nodes of
// pointers read on the host at capture time. Eagerly (no capture) the same calls must take the per-element path as before.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <cuda_runtime_api.h>
#include <cublas_v2.h>
#include "tinynv.h"
extern tinynv_device_t tinycudart_device(void);
extern int tinycudart_internal_cubin(const char*, const unsigned char*, unsigned, const void**, size_t*);
extern unsigned char gemm_cubin[]; extern unsigned int gemm_cubin_len;
static int fails;
#define CHECK(c, ...) do { int ok_ = (c); printf("  %s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
// The host-side param blobs in cublas.c against the cubin's own KPARAM_INFO: offsets of each parameter and the total.
static void layout(tinynv_module_t mod, const char *name, int n, const int *offs, int total) {
  tinynv_kernel_t k; tinynv_kernel_info_t info;
  if (tinynv_get_kernel(mod, name, &k) != TINYNV_OK || tinynv_kernel_info(k, &info) != TINYNV_OK) { CHECK(0, "%s is in the cubin", name); return; }
  int ok = info.num_params == n;
  for (int i = 0; ok && i < n; i++) ok = info.params[i].offset == offs[i];
  ok = ok && info.params[n - 1].offset + info.params[n - 1].size == total;
  CHECK(ok, "%s: %d params, offsets and size 0x%x match the host blob", name, n, total);
}
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered, so the check script reads this and stderr in the order they happened
  cublasHandle_t h; cudaStream_t st;
  if (cublasCreate(&h) != CUBLAS_STATUS_SUCCESS || cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking) || cublasSetStream(h, st) != CUBLAS_STATUS_SUCCESS) { printf("FAIL: setup\n"); return 1; }
  { const void *c; size_t n; tinynv_module_t mod;
    if (tinycudart_internal_cubin("gemm", gemm_cubin, gemm_cubin_len, &c, &n) || tinynv_module_load(tinycudart_device(), c, n, &mod) != TINYNV_OK) { printf("FAIL: cubin\n"); return 1; }
    printf("param layouts:\n");
    const int tc[14] = { 0x0, 0x8, 0x10, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40 };
    const int sc[15] = { 0x0, 0x8, 0x10, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44 };
    const int tr[7] = { 0x0, 0x8, 0x10, 0x14, 0x18, 0x1c, 0x20 };
    layout(mod, "tinyblas_gemm_f16_tc_ptrs", 14, tc, 0x44);
    layout(mod, "tinyblas_gemm_f32_tc_ptrs", 14, tc, 0x44);
    layout(mod, "tinyblas_gemm_f32_ptrs", 15, sc, 0x48);
    layout(mod, "tinyblas_trsm_rupn_f32_ptrs", 7, tr, 0x24); }
  enum { NB = 3, BIG = 65536 + 1 };   // BIG: more than gridDim.z's 65535, so the captured call goes in two chunks
  void *A, *B, *C, **Ap, **Bp, **Cp, **Gp;
  if (cudaMalloc(&A, 1 << 20) || cudaMalloc(&B, 1 << 20) || cudaMalloc(&C, 1 << 20) || cudaMalloc((void **)&Ap, NB * 8) || cudaMalloc((void **)&Bp, NB * 8)
      || cudaMalloc((void **)&Cp, NB * 8) || cudaMalloc((void **)&Gp, (size_t)BIG * 8)) { printf("FAIL: malloc\n"); return 1; }
  void *hp[NB]; for (int i = 0; i < NB; i++) hp[i] = (char *)A + i * 4096;
  cudaMemcpy(Ap, hp, sizeof hp, cudaMemcpyHostToDevice); cudaMemcpy(Bp, hp, sizeof hp, cudaMemcpyHostToDevice); cudaMemcpy(Cp, hp, sizeof hp, cudaMemcpyHostToDevice);
  const float one = 1.f, zero = 0.f;
  // eager: the per-element path, unchanged (3 launches, none of them *_ptrs)
  printf("eager begins\n");
  cublasStatus_t s = cublasGemmBatchedEx(h, CUBLAS_OP_T, CUBLAS_OP_N, 32, 32, 32, &one, (const void *const *)Ap, CUDA_R_16F, 32, (const void *const *)Bp, CUDA_R_16F, 32,
                                         &zero, (void *const *)Cp, CUDA_R_32F, 32, NB, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
  printf("eager GemmBatchedEx f16 status %d\n", (int)s); if (s) fails++;
  printf("eager ends\n");
  // poison: what the arrays hold when the pointer kernel before the call has been recorded but not run
  uint64_t poison[NB]; for (int i = 0; i < NB; i++) poison[i] = 0xdead0000ull + i * 0x100;
  cudaMemcpy(Ap, poison, sizeof poison, cudaMemcpyHostToDevice); cudaMemcpy(Bp, poison, sizeof poison, cudaMemcpyHostToDevice); cudaMemcpy(Cp, poison, sizeof poison, cudaMemcpyHostToDevice);
  printf("arrays A=%#llx B=%#llx C=%#llx G=%#llx G+65535=%#llx\n", (unsigned long long)(uintptr_t)Ap, (unsigned long long)(uintptr_t)Bp, (unsigned long long)(uintptr_t)Cp,
         (unsigned long long)(uintptr_t)Gp, (unsigned long long)(uintptr_t)(Gp + 65535));
  cudaGraph_t g = NULL; cudaGraphExec_t ge = NULL;
  printf("capture begin rc %d\n", (int)cudaStreamBeginCapture(st, cudaStreamCaptureModeRelaxed));
  cublasStatus_t s1 = cublasGemmBatchedEx(h, CUBLAS_OP_T, CUBLAS_OP_N, 32, 32, 32, &one, (const void *const *)Ap, CUDA_R_16F, 32, (const void *const *)Bp, CUDA_R_16F, 32,
                                          &zero, (void *const *)Cp, CUDA_R_32F, 32, NB, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
  cublasStatus_t s2 = cublasSgemmBatched(h, CUBLAS_OP_N, CUBLAS_OP_T, 32, 32, 32, &one, (const float *const *)Ap, 32, (const float *const *)Bp, 32, &zero, (float *const *)Cp, 32, NB);
  cublasStatus_t s3 = cublasGemmBatchedEx(h, CUBLAS_OP_T, CUBLAS_OP_N, 32, 32, 32, &one, (const void *const *)Ap, CUDA_R_16BF, 32, (const void *const *)Bp, CUDA_R_16BF, 32,
                                          &zero, (void *const *)Cp, CUDA_R_32F, 32, NB, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
  cublasStatus_t s4 = cublasGemmBatchedEx(h, CUBLAS_OP_T, CUBLAS_OP_N, 32, 32, 32, &one, (const void *const *)Gp, CUDA_R_16BF, 32, (const void *const *)Gp, CUDA_R_16BF, 32,
                                          &zero, (void *const *)Gp, CUDA_R_32F, 32, BIG, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);
  cublasStatus_t s5 = cublasStrsmBatched(h, CUBLAS_SIDE_RIGHT, CUBLAS_FILL_MODE_UPPER, CUBLAS_OP_N, CUBLAS_DIAG_NON_UNIT, 300, 70, &one,
                                         (const float *const *)Ap, 70, (float *const *)Bp, 300, NB);
  cublasStatus_t s6 = cublasGemmBatchedEx(h, CUBLAS_OP_T, CUBLAS_OP_N, 32, 32, 32, &one, (const void *const *)Ap, CUDA_R_16F, 32, (const void *const *)Bp, CUDA_R_16F, 32,
                                          &zero, (void *const *)Cp, CUDA_R_32F, 32, 0, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT);   // batch 0: nothing recorded
  printf("captured statuses %d %d %d %d %d %d\n", (int)s1, (int)s2, (int)s3, (int)s4, (int)s5, (int)s6);
  printf("capture end rc %d\n", (int)cudaStreamEndCapture(st, &g));
  int rc_inst = g ? (int)cudaGraphInstantiate(&ge, g, 0) : -1;
  int rc_launch = ge ? (int)cudaGraphLaunch(ge, st) : -1;
  printf("graph instantiate rc %d, launch rc %d\n", rc_inst, rc_launch);
  if (ge) cudaGraphExecDestroy(ge);
  if (g) cudaGraphDestroy(g);
  cublasDestroy(h);
  printf("batched_capture: %d self-check failure(s)\n", fails);
  return fails;
}

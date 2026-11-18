#!/bin/sh
# Rows for pointer-array batched cuBLAS calls under a stream capture (2026-11-19), read from test/batched_capture.c's
# output, graph.c's capture summary and the launch trace. Card-free: null device. ACCEPT-OR-REFUSE, as splitk_check.sh.
# The defect these refuse: cublasGemmBatchedEx / cublasSgemmBatched / cublasStrsmBatched read the DEVICE pointer arrays
# on the host at capture time (a direct libtinynv dtoh + sync, neither refused nor recorded) and recorded one node per
# element with those pointers frozen in - while the kernel that fills the arrays had only been recorded, not run.
set -u
BIN="${1:?usage: batched_capture_check.sh <path to batched_capture binary>}"
OUT=$(env -u TINYNV_SOCKET -u TINYNV_PCI TINYCUDART_TRACE=1 "$BIN" 2>&1); rc=$?
fails=0
row() { if [ "$2" = 0 ]; then echo "   ok:   $1"; else echo "   FAIL: $1"; fails=$((fails+1)); fi; }
has() { printf '%s\n' "$OUT" | grep -cF -- "$1" || true; }
val() { printf '%s\n' "$OUT" | sed -n "s/^arrays .*$1=\(0x[0-9a-f]*\).*/\1/p" | head -1; }
A=$(val A); B=$(val B); C=$(val C); G=$(val G); G2=$(val 'G+65535')
row "ran to the end with no self-check failure (rc $rc: param layouts against the cubin, every status 0)" "$([ $rc = 0 ] && [ "$(has "batched_capture: 0 self-check failure(s)")" = 1 ] && echo 0 || echo 1)"
EAGER=$(printf '%s\n' "$OUT" | sed -n '/^eager begins$/,/^eager ends$/p')
row "eager (no capture): the per-element path as before - 3 launches, no *_ptrs kernel" "$([ "$(printf '%s\n' "$EAGER" | grep -c '^\[trace\] launch ')" = 3 ] && [ "$(printf '%s\n' "$EAGER" | grep -c '_ptrs')" = 0 ] && echo 0 || echo 1)"
row "capture begin, end, instantiate and launch all return 0" "$([ "$(has 'capture begin rc 0')" = 1 ] && [ "$(has 'capture end rc 0')" = 1 ] && [ "$(has 'graph instantiate rc 0, launch rc 0')" = 1 ] && echo 0 || echo 1)"
CAP='graphs: capture 1 holds 6 launches, 0 copies, 0 fills, 0 strided copies; first: tinyblas_gemm_f16_tc_ptrs tinyblas_gemm_f32_tc_ptrs tinyblas_gemm_f32_ptrs ... last: tinyblas_trsm_rupn_f32_ptrs tinyblas_gemm_f32_ptrs tinyblas_gemm_f32_ptrs'
row "the graph holds ONE *_ptrs node per call (f16, f32, bf16, bf16 in 2 chunks, trsm; batch 0 none), not one per element" "$([ "$(has "$CAP")" = 1 ] && echo 0 || echo 1)"
row "its one replay issues all 6 nodes" "$([ "$(has 'graphs: 1 captures recorded 6 nodes; 1 graph launches replayed 6 nodes')" = 1 ] && echo 0 || echo 1)"
row "GemmBatchedEx f16 node: grid z = batch 3, holding the ARRAYS' device addresses" "$([ -n "$A" ] && [ "$(has "launch tinyblas_gemm_f16_tc_ptrs grid=(1,1,3) block=(128,1,1) m=32 n=32 k=32 in=1 out=0 Aarray=$A Barray=$B Carray=$C")" = 1 ] && echo 0 || echo 1)"
row "SgemmBatched f32 node: the TF32 kernel, the arrays' addresses" "$([ -n "$A" ] && [ "$(has "launch tinyblas_gemm_f32_tc_ptrs grid=(1,1,3) block=(128,1,1) m=32 n=32 k=32 in=0 out=0 Aarray=$A Barray=$B Carray=$C")" = 1 ] && echo 0 || echo 1)"
row "GemmBatchedEx bf16 node: the scalar kernel, the arrays' addresses" "$([ -n "$A" ] && [ "$(has "launch tinyblas_gemm_f32_ptrs grid=(2,2,3) block=(16,16,1) m=32 n=32 k=32 in=2 out=0 Aarray=$A Barray=$B Carray=$C")" = 1 ] && echo 0 || echo 1)"
row "batch 65537: two nodes, z 65535 at the array base and z 2 at base + 65535 pointers" "$([ -n "$G" ] && [ "$(has "grid=(2,2,65535) block=(16,16,1) m=32 n=32 k=32 in=2 out=0 Aarray=$G Barray=$G Carray=$G")" = 1 ] && [ "$(has "grid=(2,2,2) block=(16,16,1) m=32 n=32 k=32 in=2 out=0 Aarray=$G2 Barray=$G2 Carray=$G2")" = 1 ] && echo 0 || echo 1)"
row "StrsmBatched node: grid z = batch 3, the arrays' addresses" "$([ -n "$A" ] && [ "$(has "launch tinyblas_trsm_rupn_f32_ptrs grid=(2,1,3) block=(256,1,1) k=300 n=70 Aarray=$A Xarray=$B")" = 1 ] && echo 0 || echo 1)"
[ $fails = 0 ] || { printf '%s\n' "--- output:" "$OUT" | grep -v -e '^tinynv: would launch' -e '^\[trace\] \(malloc\|free\|memcpy\)' | head -60; }
echo "batched_capture_check: $fails failure(s)"
exit $fails

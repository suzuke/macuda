# GA106 tensor unlock: register and GEMM observations

Tested 2026-11-18 on macuda `c642a73`, RTX 3060 12 GB (GA106, PCI device `10de:2544`) in a Razer Core X V2 over Thunderbolt. The 610.43.03 booter bundle was SHA-256 verified against the hash in `tensor_unlock.c`; normal GSP firmware was not upgraded. The same arm64 benchmark binary (SHA-256 `b6e0e2ac2de21fd3e265bf734719f3848719c5d5281218bdbb0fc14d2f5c7470`) and same sm_86 cubin were used for both states. This binary was built from [`cuda-shim/test/tensor_unlock_bench.c`](../../cuda-shim/test/tensor_unlock_bench.c), whose SHA-256 was `9691acf6d038072bdc50405ec57c7519f75c8367b5b302d8979d22076ba796d2`.

## Device state

Before testing, the enclosure was power-cycled. Read-only MMIO after re-enumeration showed PLM `0xffffff8f`, SS0 `0x51000104`, SS1 `0x00000005`, and FEAT_READOUT_1 (`0x823818`) `0x00400080`. The stock run left `TINYNV_TENSOR_UNLOCK` unset, not `0`.

On the unlocked run, the booter opened PLM, SS0/SS1 read back `0x88888888`/`0x8`, and the driver performed a cold reset and retried. Attempt 2 saw the same selector values, skipped the exploit, and brought firmware up. A subsequent read showed SS0/SS1 unchanged and READOUT_1 `0`. PLM read `0xffffffef` after the attempted restore, not the original `0xffffff8f`. Each process unloaded GSP successfully; the final preflight passed. The card remained unlocked at the end.

Earlier, `TINYNV_TENSOR_UNLOCK=0` did not restore the selectors: the host's attempt to open PLM was rejected and SS0/SS1 remained `0x88888888`/`0x8`. It is not a validated way to obtain a stock baseline on this card.

## Method and results

Each case was a separate process and ran stock first, then unlocked, in the order shown below. The benchmark used three warmups, followed by seven timed batches of five GEMMs. `clock_gettime` covers host submission and device synchronization, but not data transfers. Each output element of the last GEMM was compared with an analytic reference, absolute tolerance 0.05; inputs were a deterministic pattern exactly representable as FP16 and TF32. All cases reported zero mismatches and maximum absolute error zero. This is a limited correctness check, not a random-input or numerical-stability suite.

Reported numbers are medians of the seven batches in TFLOP/s, counting 2N³ operations per GEMM. FP16 uses FP32 accumulation and FP32 output; TF32 uses FP32 input and output.

| Case | Stock | Unlocked | Ratio | Stock range | Unlocked range |
| --- | ---: | ---: | ---: | ---: | ---: |
| FP16 2048³ | 13.411 | 14.823 | 1.105× | 13.372–13.474 | 14.810–14.869 |
| FP16 4096³ | 20.689 | 30.054 | 1.453× | 20.607–20.813 | 29.838–30.779 |
| FP16 8192³ | 21.086 | 30.402 | 1.442× | 21.080–21.090 | 30.375–30.518 |
| TF32 4096³ | 3.099 | 3.395 | 1.096× | 3.098–3.107 | 3.395–3.411 |

The seven measurements, in acquisition order (TFLOP/s):

| Case | Stock | Unlocked |
| --- | --- | --- |
| FP16 2048³ | 13.417580, 13.371629, 13.371629, 13.396654, 13.411295, 13.436469, 13.474407 | 14.835811, 14.828128, 14.869196, 14.823011, 14.810232, 14.823011, 14.817897 |
| FP16 4096³ | 20.606776, 20.810841, 20.812731, 20.723606, 20.659434, 20.670620, 20.689290 | 30.475621, 30.778643, 30.059699, 29.837817, 29.998025, 30.008505, 30.054440 |
| FP16 8192³ | 21.079756, 21.089217, 21.083879, 21.088570, 21.086467, 21.081130, 21.089621 | 30.517576, 30.391551, 30.409203, 30.426033, 30.375095, 30.384665, 30.401972 |
| TF32 4096³ | 3.107398, 3.100919, 3.098626, 3.098333, 3.098584, 3.099060, 3.099032 | 3.410684, 3.394711, 3.395416, 3.394879, 3.395130, 3.394829, 3.395315 |

The 2048 FP16 kernel was `tinyblas_gemm_f16_tc2s_nn`; 4096 and 8192 used `tinyblas_gemm_f16_tc2_nn`; TF32 used `tinyblas_gemm_f32_tc`. Matched stock/unlocked pairs loaded the same kernel and cubin (FNV-1a64 `ae06585909b4bb93`). Raw local logs are `logs/shim-probe-20261118-213035.log`, `213040`, `213045`, `213053` (stock), and `213106`, `213114`, `213120`, `213127` (unlocked); the latter is the suffix of the same filename. `logs/` is ignored and those files are not included in the commit.

This is one power-cycle and one process per condition, not seven independent power cycles. Clocks, power and temperature were not recorded; the order was not randomized. BF16 was not measured: the local SM86 cubin failed the offline cuBLAS contract because the BF16 kernel was missing. TF32 and small-matrix performance may be limited by this implementation, so the ratios do not establish hardware peak throughput.

## Upstream comparison

[nv-uefi-unlocker issue #70](https://github.com/physicsdolphin/nv-uefi-unlocker/issues/70) reports RTX 3060 12 GB GA106 `10de:2504`, Windows driver 617.42, PyTorch 2.12.1+cu130, FP32 accumulation with reduced-precision reduction disabled. FP16 GEMM was 25.37→45.79 TFLOP/s at 4096³ (1.80×), and 25.74→46.90 at 8192³ (1.82×). That report uses a different card ID, OS, driver, direct-attached host, GEMM implementation and potentially different timing/inputs. Both show a gain, but the 1.44–1.45× macuda result cannot be attributed to an incomplete unlock solely because it is below upstream's 1.80–1.82×. Compare with the same benchmark method and collect clock/power telemetry before attributing the difference.

## Reproduction

With the sm_86 shim built, link the benchmark using the SDK and library paths appropriate to the host (the following were used here):

```sh
make -C cuda-shim -j4 BUILD=build/shim-sm_86 CUBIN_ARCH=sm_86
/opt/homebrew/opt/llvm/bin/clang -isysroot "$(sh cuda-shim/build/sdk.sh)" -O2 \
  -Icuda-shim/cuda-13/include cuda-shim/test/tensor_unlock_bench.c \
  cuda-shim/build/shim-sm_86/libtinycudart.a cuda-shim/build/shim-sm_86/libtinycublas.a \
  cuda-shim/build/shim-sm_86/nv/libtinynv.a /opt/homebrew/opt/llvm/lib/libLLVMDemangle.a \
  -lc++ -lm -o /tmp/macuda_tensor_unlock_rigorous
```

After a confirmed enclosure power cycle and read-only selector inspection, run `env -u TINYNV_TENSOR_UNLOCK sh tools/nv_shim_step.sh B probe /tmp/macuda_tensor_unlock_rigorous f16 4096`. Run the corresponding unlocked case with `env TINYNV_TENSOR_UNLOCK=1` in front of the same command. Repeat for `f16 2048`, `f16 8192`, and `tf32 4096`, recording state, temperatures and clocks where available. Follow preflight; do not assume a function-level reset clears the override.

The successful path was exercised, not every failure path. In particular, a timeout may release booter/payload buffers without first proving SEC2 stopped; an all-ones PLM read is not corroborated against a live FEAT block; and the attempted PLM restore is not verified. These remain implementation issues to investigate before treating the mechanism as robust.

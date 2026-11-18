# GA106: restoring fused speed selectors without an enclosure power cycle

2026-11-18: an experimental modification in a detached worktree reopened the FEAT PLM via the existing 610.43.03 SEC2/V67 path, cleared the speed-selector override enable bits, and then performed the driver's FLR and full firmware reinitialization. On this RTX 3060 GA106 (`10de:2544`, Razer Core X V2), two restricted→unlocked cycles completed without powering the enclosure off. This is a proof of concept, not production support.

The experiment started from the unlocked selectors `SS0=0x88888888`, `SS1=0x8`, `READOUT_1=0`. The patch preserves the low three bits of each SS0 nibble and clears its high bit; it clears bit 3 of SS1. With the starting values this writes `SS0=0`, `SS1=0`. The first round read back those values after FLR and `READOUT_1=0x00400080`, matching the fused readout observed after a genuine enclosure power cycle. **Zero is not an exact restoration of the cold selector shadow**, which read `0x51000104` and `0x5`; the evidence is that disabling the override restored this card's readout and the tested GEMM speed. The second round likewise showed zero selectors across FLR, though READOUT_1 was not independently sampled during that second restricted phase.

The patch was built separately and is recorded as [tensor-reset-ga106-experiment.patch](tensor-reset-ga106-experiment.patch). It does not modify the main checkout's driver. The experimental benchmark binary identified itself as `c642a73` because it linked the existing build-ID object; its SHA-256 was `d7f94015c9d335ca5a88690cbae3d8dd7017ac490ed608168528d63e79b7720e`. The ordinary unlock benchmark binary SHA-256 was `b6e0e2ac2de21fd3e265bf734719f3848719c5d5281218bdbb0fc14d2f5c7470`. Both used the same GEMM cubin.

| Step | Mode | 4096³ FP16/FP32-accumulate median TFLOP/s | Local log |
| --- | --- | ---: | --- |
| 1 | Experimental override clear | 20.672486 | `logs/shim-probe-20261118-213638.log` |
| 2 | Existing unlock | 29.953568 | `logs/shim-probe-20261118-213658.log` |
| 3 | Experimental override clear | 20.816514 | `logs/shim-probe-20261118-213713.log` |
| 4 | Existing unlock | 30.179832 | `logs/shim-probe-20261118-213724.log` |

All four runs checked 16,777,216 outputs, with zero mismatches and max absolute error zero. Each median covers seven host-timed batches of five GEMMs after three warmups, using the deterministic pattern in `cuda-shim/test/tensor_unlock_bench.c`. Prior true-power-cycle stock median was 20.689290 TFLOP/s. See [the preceding experiment](../bench/tensor-unlock-ga106-20261118.md) for methodology and limits. Logs are ignored by Git; this report records the observed values, not the raw log files.

In both override-clear runs the PLM flipped, the selectors read zero, FLR and full reinitialization completed, and attempt 2 skipped another exploit because the override bits were clear. Both subsequent unlocks successfully repeated the normal exploit/FLR path. At the end SS0/SS1 were `0x88888888`/`0x8`, READOUT_1 was `0`, firmware had unloaded, and preflight passed.

Upstream's [transaction code](https://github.com/physicsdolphin/nv-uefi-unlocker/blob/a0da84f207f29501ea395384605ca0ee3b40c7a4/loader/src/tx.rs) uses the GA102 PLM-opening payload followed by host selector writes; its PLM relock closes the gate rather than reverting SS. Its [diagnostic documentation](https://github.com/physicsdolphin/nv-uefi-unlocker/blob/a0da84f207f29501ea395384605ca0ee3b40c7a4/tools/ssdiag/README.md) describes reapplying unlock selectors when the gate remains open, not this exact restore procedure. Upstream does not establish this experiment's behavior on this platform.

At the time of this first experiment, its patch had unsafe DMA-buffer cleanup on timeout, warning-only selector readback, no PLM restoration check, and no distinction between an all-ones dead FEAT block and an open PLM. The follow-up below records the subsequent integration and validation.

## Integrated state-machine follow-up

After the experiment, the transaction was integrated into `libtinynv` with explicit lifetime and retry state. SEC2 image, payload and metadata buffers remain allocated until a successful cold reset; if reset fails, teardown first disables bus mastering before releasing them. The PLM poll now has a five-second deadline and distinguishes an unreadable FEAT block, SEC2 halting, and timeout from an open PLM. Selector writes and PLM restoration are read back; a failed verification attempts to restore the original selectors and reports whether rollback was verified. The next firmware init checks that the requested selectors survived FLR before proceeding.

The pure state tests cover poll success, timeout, SEC2 halt, FEAT sentinel values, selector readback mismatches, the known GA106 PLM readback (`0xffffff8f` → `0xffffffef`), and retry limits: **26 checks, zero failures**. The updated `sm_86` shim built successfully with `-Werror`.

The updated binary (`b9e896c-dirty`) ran two unlock/clear cycles on the same powered-on RTX 3060. Each clear and unlock fired the booter, changed the selectors, cold-reset, then verified the selector values on firmware attempt 2. The clear path read `SS0=0`, `SS1=0`, `READOUT_1=0x00400080`; unlock read `SS0=0x88888888`, `SS1=0x8`, `READOUT_1=0`. The card finished unlocked, with the GSP unloaded and final preflight clean.

| FP16 GEMM | Restricted median | Unlocked median | Change | Correctness |
| --- | ---: | ---: | ---: | --- |
| 2048³ | 13.940 TFLOP/s | 15.185 TFLOP/s | +8.9% | 0 / 4,194,304 mismatches in both states |
| 4096³ | 20.719 TFLOP/s | 29.927 TFLOP/s | +44.4% | 0 / 16,777,216 mismatches in both states |

Each median covers seven timed batches of five GEMMs after three warmups, using the same binary and cubin within each pair. The new 4096³ ratio reproduces the earlier macuda result (~1.44×); it remains below the upstream issue #70 result (~1.80×), while 2048³ is a different, smaller-kernel regime with a much smaller gain. The upstream and local measurements still use different hosts, drivers, and benchmark implementations, so the gap does not by itself establish an unlock defect. Logs: `logs/shim-probe-20261118-221843.log`, `221904`, `221935`, and `221947` (ignored by Git).

These tests validate the success path and pure decision helpers. They did not force a real SEC2 timeout, corrupt an MMIO read on hardware, or force cold-reset failure; those cases are covered only at the state-classification level or by code-path review because provoking them on the live card could leave the device requiring re-enumeration. Evidence remains limited to this GA106 and firmware bundle. The procedure still depends on the undocumented FEAT registers, SEC2 payload, FLR, and full firmware reinitialization; it is not a live-context toggle or proof for every GA10x.

# Roadmap

Updated 2026-11-19, at main `0e4b06f`. What is in flight, what is next, and what each step needs. Gates are explicit:
"card" means the step runs on the GPU, "fault" means it deliberately breaks the card's session and can panic macOS
(owner present and agreeing, every time), "outward" means it publishes or sends something (owner's go first).

## In flight

- **M4, the out-of-process watchdog** (branch `feat/dart-m4`). One watchdog process per CUDA program clears bus
  mastering when the client dies without a clean teardown (H2: client SIGKILLed while the server lives) or when the
  server dies while the client is idle - the two cases M2/M3 cannot see. Clean exits make no connection to the server; a
  lock keeps the client's own rescue and the watchdog from starting two servers. Offline tests first, then a normal-path
  run on the card (no leftover watchdog, back-to-back steps not refused), then merge.

## Next: finishing the DART guard

Background and evidence: [stability hypotheses](driver/stability-hypotheses.md), "2026-11-18 DART panic and what
followed". Step B (2026-11-19) cut a server-death storm from 129 faults over 32 s (and a panic) to 6 faults in 200 ms.

| # | Item | Gate |
|---|---|---|
| 1 | M4 fault test on the card: client `kill -9` with the server alive; server death while the client is idle | card, fault |
| 2 | The server the bmclear helper starts runs without the M6 wrapper (no exit status, no DART snapshot): start it through `tools/tinygpu-server.sh` | none |
| 3 | M5, make the server harder to kill by accident (`kill_tinygpu.sh`, tinygrad's `pkill -f TinyGPU` path beyond the sentinel file) | none |
| 4 | Report the real fix upstream: the dext should revoke bus mastering before `CompleteDMA` on session close. Only that closes the H2/H10 class; macuda can only shorten the window | outward |

Whether a ~200 ms storm always stays clear of the `AppleT8110DART` panic is unknown (closed source, no threshold data).

## Then: releases

5. **Prebuilt releases.** The bmclear helper's path is baked in as an absolute build-tree path
   (`-DTINYNV_BMCLEAR_DEFAULT`, libtinynv/Makefile); make it relocatable (next to the dylib) first, then decide the
   binary set and architectures (sm_86 / sm_89 / sm_120). Gate: outward.
6. **RTX 4090 and 5090 checks** when the cards are available: split-K and pointer-array batched GEMM under graph
   capture were verified on the 3060 (sm_86) only. Gate: card.

## Low priority or paused

7. **TC=0 VAE timeout.** Only with tensor cores turned off (`TINYCUBLAS_TC=0`); the default path is unaffected.
8. **FP8 GEMM** (worktree `macuda-fp8`, no changes yet). Paused: no F8_E4M3 model in use, and ggml falls back correctly
   (it does quantize activations before asking the heuristic, which is wasted work). Measure F8_E4M3 vs Q8_0 prefill on a
   5090 first; write the e4m3 kernel only if the gap is worth it.

## Housekeeping done 2026-11-19

Removed the merged worktrees `macuda-llama-up`, `macuda-memgetinfo`, `macuda-tensor-unlock` (their branches were fully
in main) and pruned the stale `macuda-tensor-reset-research`. Their local logs were kept under
`logs/archive-worktrees/` (not in git).

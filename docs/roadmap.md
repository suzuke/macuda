# Roadmap

Updated 2026-11-19, at main `7d3177c`. What is in flight, what is next, and what each step needs. Gates are explicit:
"card" means the step runs on the GPU, "fault" means it deliberately breaks the card's session and can panic macOS
(owner present and agreeing, every time), "outward" means it publishes or sends something (owner's go first).

## Done 2026-11-19: M4, the out-of-process watchdog (main `a7a0622`)

One `tinynv-bmclear --watch` per TinyGPU session clears bus mastering when the last process holding the session goes
without a teardown that read mastering back off (kill -9, crash, exit without teardown, closed stdio, a fork that
re-arms), and when the server dies under an armed client, retrying with backoff. Clean exits make no connection to the
server. Four fresh-context review rounds (three refuted, then confirmed); 132 offline checks; normal path on the 3060
(opverify 490/490, server probe byte-identical, no leftover watchdog, no extra connections). Not yet fault-tested on the
card (item 1 below). Documented gaps, judged unlikely in real use:
- after the client is gone there is one clear of at most ~10 s: a server that takes longer to finish the dead session,
  or a replacement app that takes over ~20 s to listen, leaves mastering on;
- an idle client's retry that lands while another client is being served writes nothing, but its closed connection
  sits in the server's backlog until that session ends (a third program is refused meanwhile);
- the retry has no attempt limit while the client lives (every 80 s at most); a manual `tinygpu-server.sh start` within
  the ~50 ms before the app is exec'd could race a retry into a second server (not reproduced);
- if the pipe cannot be moved above fd 2 (descriptor table full with stdio closed) the session runs without a watchdog;
- the watchdog is the client's child for the client's whole life (a host looping on `wait()` until ECHILD would block).

## Next: finishing the DART guard

Background and evidence: [stability hypotheses](driver/stability-hypotheses.md), "2026-11-18 DART panic and what
followed". Step B (2026-11-19) cut a server-death storm from 129 faults over 32 s (and a panic) to 6 faults in 200 ms.

| # | Item | Gate |
|---|---|---|
| 1 | M4 fault test on the card: client `kill -9` with the server alive (H2); server death while the client is idle | card, fault |
| 2 | ~~A server the bmclear helper starts runs without the M6 wrapper~~ Done 2026-11-19 (`7d3177c`): it starts through `tools/tinygpu-server.sh start` (script(1), exit status, both DART snapshots), falling back to a direct spawn if the script is missing. Gap: a ~20-50 ms window before script(1) execs the app in which a second, non-waiting helper could start another server (needs a watchdog timing out in that window; judged unrealistic) | none |
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

#ifndef TINYNV_TENSOR_UNLOCK_H
#define TINYNV_TENSOR_UNLOCK_H
#include "gpu.h"

// Attempt to open the FEAT PLM and write the speed-select override registers on GA10x,
// unlocking full tensor-core throughput. Must be called after FWSEC has placed WPR2
// (and init_wpr_meta has run) but before the normal booter_load.
//
// On success the PLM is open and SS0/SS1 carry the full-speed override; on failure (non-fatal)
// a diagnostic is printed and the caller proceeds with the normal boot — the GPU is not bricked,
// only the speed lock stays in effect.
//
// Volatile: PLM and SS writes are in the AON domain and survive FLR but are lost on power loss.
//
// Controlled by TINYNV_TENSOR_UNLOCK=1 (default off).
int tinynv_tensor_unlock(tinynv_gpu_t *g);

#endif

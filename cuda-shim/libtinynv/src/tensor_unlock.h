#ifndef TINYNV_TENSOR_UNLOCK_H
#define TINYNV_TENSOR_UNLOCK_H
#include "gpu.h"

// The optional GA10x selector transaction runs during VBIOS boot after FWSEC.
// Any transaction which touches SEC2 leaves its buffers in g->tensor until the
// caller has cold-reset the device (or quiesced bus mastering during teardown).
// TINYNV_TENSOR_UNLOCK=1 enables full-speed overrides; =0 clears their enable bits.
int tinynv_tensor_unlock(tinynv_gpu_t *g);

// Call only after a successful cold reset or bus mastering has been disabled.
void tinynv_tensor_cleanup(tinynv_gpu_t *g);

#endif

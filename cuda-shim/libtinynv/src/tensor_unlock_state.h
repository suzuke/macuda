#ifndef TINYNV_TENSOR_UNLOCK_STATE_H
#define TINYNV_TENSOR_UNLOCK_STATE_H
#include <stdint.h>

#define TINYNV_FEAT_BADF 0xbadf5040u

enum tinynv_tensor_poll_result {
  TINYNV_TENSOR_POLL_WAIT,
  TINYNV_TENSOR_POLL_OPEN,
  TINYNV_TENSOR_POLL_FEAT_UNREADABLE,
  TINYNV_TENSOR_POLL_HALTED,
  TINYNV_TENSOR_POLL_TIMEOUT
};

static inline int tinynv_feat_block_responds(uint32_t witness) {
  return witness != UINT32_MAX && witness != TINYNV_FEAT_BADF;
}

static inline enum tinynv_tensor_poll_result tinynv_tensor_poll_status(
    uint32_t plm, uint32_t witness, int halted, int timed_out) {
  if (!tinynv_feat_block_responds(witness) || plm == TINYNV_FEAT_BADF)
    return TINYNV_TENSOR_POLL_FEAT_UNREADABLE;
  if (plm == UINT32_MAX) return TINYNV_TENSOR_POLL_OPEN;
  if (halted) return TINYNV_TENSOR_POLL_HALTED;
  if (timed_out) return TINYNV_TENSOR_POLL_TIMEOUT;
  return TINYNV_TENSOR_POLL_WAIT;
}

static inline int tinynv_tensor_selectors_match(uint32_t want0, uint32_t want1,
                                                uint32_t got0, uint32_t got1,
                                                uint32_t readout) {
  return got0 == want0 && got1 == want1 &&
         readout != UINT32_MAX && readout != TINYNV_FEAT_BADF;
}

static inline int tinynv_tensor_plm_restore_matches(uint32_t before, uint32_t after) {
  return after == before || (before == 0xffffff8fu && after == 0xffffffefu);
}

static inline int tinynv_tensor_retry_after_reset(int pending, int retry,
                                                   int attempt, int max_attempts) {
  return pending && retry && attempt < max_attempts;
}

#endif

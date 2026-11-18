#include "../src/tensor_unlock_state.h"
#include <stdio.h>

static int checks, failures;
#define CHECK(expr, label) do { checks++; if (!(expr)) { failures++; printf("FAIL: %s\n", label); } } while (0)

int main(void) {
  CHECK(tinynv_feat_block_responds(0x51000104), "cold FEAT witness responds");
  CHECK(tinynv_feat_block_responds(0x88888888), "unlocked FEAT witness responds");
  CHECK(!tinynv_feat_block_responds(UINT32_MAX), "all-ones FEAT witness rejected");
  CHECK(!tinynv_feat_block_responds(TINYNV_FEAT_BADF), "BADF FEAT witness rejected");

  CHECK(tinynv_tensor_poll_status(0xffffff8f, 0x51000104, 0, 0) == TINYNV_TENSOR_POLL_WAIT,
        "closed PLM continues polling");
  CHECK(tinynv_tensor_poll_status(UINT32_MAX, 0x51000104, 0, 0) == TINYNV_TENSOR_POLL_OPEN,
        "open PLM wins on final read");
  CHECK(tinynv_tensor_poll_status(TINYNV_FEAT_BADF, 0x51000104, 0, 0) == TINYNV_TENSOR_POLL_FEAT_UNREADABLE,
        "BADF PLM is not mistaken for an open gate");
  CHECK(tinynv_tensor_poll_status(0xffffff8f, UINT32_MAX, 0, 0) == TINYNV_TENSOR_POLL_FEAT_UNREADABLE,
        "unreadable witness rejects all-ones PLM");
  CHECK(tinynv_tensor_poll_status(0xffffff8f, 0x51000104, 1, 0) == TINYNV_TENSOR_POLL_HALTED,
        "halted booter fails before timeout");
  CHECK(tinynv_tensor_poll_status(0xffffff8f, 0x51000104, 0, 1) == TINYNV_TENSOR_POLL_TIMEOUT,
        "closed PLM expires at deadline");
  CHECK(tinynv_tensor_poll_status(UINT32_MAX, UINT32_MAX, 0, 1) == TINYNV_TENSOR_POLL_FEAT_UNREADABLE,
        "unreadable MMIO is not accepted at deadline");

  CHECK(tinynv_tensor_selectors_match(0x88888888, 8, 0x88888888, 8, 0), "unlock selectors/readout accepted");
  CHECK(tinynv_tensor_selectors_match(0, 0, 0, 0, 0x00400080), "disabled selectors/fused readout accepted");
  CHECK(!tinynv_tensor_selectors_match(0x88888888, 8, 0x88888888, 0, 0), "SS1 readback mismatch rejected");
  CHECK(!tinynv_tensor_selectors_match(0, 0, 0x88888888, 0, 0x00400080), "SS0 readback mismatch rejected");
  CHECK(!tinynv_tensor_selectors_match(0, 0, 0, 0, UINT32_MAX), "unreadable readout rejected");
  CHECK(!tinynv_tensor_selectors_match(0, 0, 0, 0, TINYNV_FEAT_BADF), "BADF readout rejected");

  CHECK(tinynv_tensor_plm_restore_matches(0xffffffef, 0xffffffef), "unchanged PLM accepted");
  CHECK(tinynv_tensor_plm_restore_matches(0xffffff8f, 0xffffffef), "observed GA106 sticky PLM accepted");
  CHECK(!tinynv_tensor_plm_restore_matches(0xffffff8f, 0xffffffff), "unexpected open PLM rejected");
  CHECK(!tinynv_tensor_plm_restore_matches(0xffffffef, 0xffffff8f), "unexpectedly closed PLM rejected");
  CHECK(!tinynv_tensor_plm_restore_matches(0xffffff8f, TINYNV_FEAT_BADF), "BADF PLM readback rejected");

  CHECK(tinynv_tensor_retry_after_reset(1, 1, 1, 3), "pending transaction retries before limit");
  CHECK(!tinynv_tensor_retry_after_reset(1, 1, 3, 3), "pending transaction stops at limit");
  CHECK(!tinynv_tensor_retry_after_reset(1, 0, 1, 3), "non-retryable failure stops");
  CHECK(!tinynv_tensor_retry_after_reset(0, 1, 1, 3), "no pending transaction does not retry");

  printf("tensor unlock state: %d checks, %d failure(s)\n", checks, failures);
  return failures ? 1 : 0;
}

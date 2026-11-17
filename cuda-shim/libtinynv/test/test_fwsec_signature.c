#include "../src/fwsec_signature.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  unsigned version = 99, index = 99;
  /* Actual 10de:2544 regression: fuse 3 selects index 2, not last index 3. */
  assert(!tinynv_fwsec_signature(3, 15, 4, 4*384, 100, 4096, &version, &index));
  assert(version == 2 && index == 2);
  /* Sparse masks: version and packed signature index differ. */
  assert(!tinynv_fwsec_signature(3, 5, 2, 768, 100, 4096, &version, &index));
  assert(version == 2 && index == 1);
  assert(!tinynv_fwsec_signature(0, 1, 1, 384, 0, 384, &version, &index));
  assert(version == 0 && index == 0);
  assert(!tinynv_fwsec_signature(0x4000, 0x8000, 1, 384, 0, 384, &version, &index));
  assert(version == 15 && index == 0);
  /* Fuse version unavailable, invalid count, truncation and overflow. */
  assert(tinynv_fwsec_signature(3, 3, 2, 768, 0, 4096, &version, &index));
  assert(tinynv_fwsec_signature(0x8000, 0xffff, 16, 6144, 0, 8192, &version, &index));
  assert(tinynv_fwsec_signature(UINT32_MAX, 0xffff, 16, 6144, 0, 8192, &version, &index));
  assert(tinynv_fwsec_signature(3, 15, 2, 1536, 0, 4096, &version, &index));
  assert(tinynv_fwsec_signature(0, 1, 0, 384, 0, 4096, &version, &index));
  assert(tinynv_fwsec_signature(0, 0, 1, 384, 0, 4096, &version, &index));
  assert(tinynv_fwsec_signature(0, 1, 1, 383, 0, 4096, &version, &index));
  assert(tinynv_fwsec_signature(0, 1, 1, 384, 1, 384, &version, &index));
  assert(tinynv_fwsec_signature(0, 1, 1, 384, SIZE_MAX, 4096, &version, &index));
  puts("FWSEC: 13 offline cases passed");
  return 0;
}

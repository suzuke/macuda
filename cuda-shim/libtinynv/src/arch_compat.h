// Can a cubin's machine code run on this card? Pure, so a card-free test can hold every case (test/test_arch_compat.c).
//
// The card's compute capability comes from NV_PMC_BOOT_42's architecture field, which dev.c reads before the firmware
// boots, so the answer is known before the first module goes across. The cubin's comes from its ELF flags (cubin.c:
// sm_arch, 86 for sm_86, 120 for sm_120 and sm_120a). SASS runs on a card of the SAME major version whose minor is the
// same or newer: sm_86 code runs on an RTX 3060 (8.6) and on Ada (8.9), never on Blackwell (12.0); sm_120 code never
// runs on Ampere. Before this check a wrong-architecture binary was loaded and launched without complaint.
#ifndef TINYNV_ARCH_COMPAT_H
#define TINYNV_ARCH_COMPAT_H
#include <stdint.h>

// Compute capability as major*10+minor for a BOOT_42 architecture this driver boots, or 0 for one it does not know.
// GA10x (0x17) are all 8.6: GA100, the 8.0 part, is a data-centre chip this driver has no firmware path for.
static inline uint32_t tinynv_arch_cc(uint32_t boot42_arch) {
  switch (boot42_arch) {
    case 0x17: return 86;    // Ampere GA10x
    case 0x19: return 89;    // Ada AD10x
    case 0x1b: return 120;   // Blackwell GB20x
    default: return 0;
  }
}

// 1 when code compiled for sm_<cubin_sm> runs on a card of compute capability <card_cc> (both major*10+minor), else 0.
// An unknown on either side is 0: refusing is recoverable, running the wrong instruction set is not.
static inline int tinynv_sm_fits(uint32_t cubin_sm, uint32_t card_cc) {
  if (!cubin_sm || !card_cc) return 0;
  return cubin_sm / 10 == card_cc / 10 && cubin_sm % 10 <= card_cc % 10;
}
#endif

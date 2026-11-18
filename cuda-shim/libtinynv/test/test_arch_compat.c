// The architecture check that stands between a cubin and the card: which code may run where (src/arch_compat.h).
#include "../src/arch_compat.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
  // the card's capability from BOOT_42
  CHECK(tinynv_arch_cc(0x17) == 86, "Ampere GA10x is 8.6");
  CHECK(tinynv_arch_cc(0x19) == 89, "Ada AD10x is 8.9");
  CHECK(tinynv_arch_cc(0x1b) == 120, "Blackwell GB20x is 12.0");
  CHECK(tinynv_arch_cc(0x18) == 0 && tinynv_arch_cc(0) == 0 && tinynv_arch_cc(0xff) == 0, "an unknown architecture has no capability");

  // what runs where: the binaries this tree builds, against the cards it boots
  CHECK(tinynv_sm_fits(86, 86), "sm_86 on an RTX 3060");
  CHECK(tinynv_sm_fits(86, 89), "sm_86 on Ada (the README's sm_89 build runs sm_86 code)");
  CHECK(tinynv_sm_fits(120, 120), "sm_120 on Blackwell");
  CHECK(!tinynv_sm_fits(120, 86), "sm_120 refused on Ampere");
  CHECK(!tinynv_sm_fits(120, 89), "sm_120 refused on Ada");
  CHECK(!tinynv_sm_fits(86, 120), "sm_86 refused on Blackwell: SASS does not cross a major version");
  CHECK(!tinynv_sm_fits(89, 86), "sm_89 refused on an 8.6 card: a newer minor is not backwards compatible");
  CHECK(tinynv_sm_fits(80, 86), "sm_80 runs on 8.6 (same major, older minor)");
  CHECK(!tinynv_sm_fits(90, 86) && !tinynv_sm_fits(90, 120), "sm_90 runs on neither");

  // unknowns refuse
  CHECK(!tinynv_sm_fits(0, 86), "a cubin with no architecture is refused");
  CHECK(!tinynv_sm_fits(86, 0), "a card with no known capability refuses everything");

  printf(fails ? "%d of %d checks failed\n" : "architecture compatibility: all %d checks passed\n", fails ? fails : checks, checks);
  return fails != 0;
}

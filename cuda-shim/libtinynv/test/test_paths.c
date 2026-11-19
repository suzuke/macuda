// The paths a release finds beside itself (src/exe_path.h), printed for test/test_paths.sh to judge: that script copies
// this binary into temp <prefix>/bin directories and checks what it says. Nothing is judged here, so there is no
// verdict to misplace; the script counts its checks.
//
//   helper <path>     what tg_rescue and tg_watch would start: TINYNV_BMCLEAR_HELPER, else the stored lookup
//   firmware <dir>    tinynv_fw_root(), asked twice so a second lookup would show as a second line on stderr
//   same <0|1>        whether a second tinynv_pci_bmclear_helper() returned the same stored path
#include "fw.h"
#include "tinynv_pci.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *a = tinynv_pci_bmclear_helper(), *b = tinynv_pci_bmclear_helper();
  const char *h = getenv("TINYNV_BMCLEAR_HELPER");
  if (!h || !*h) h = a;
  const char *f = tinynv_fw_root();
  tinynv_fw_root();
  printf("helper %s\nfirmware %s\nsame %d\n", h, f, a == b && !strcmp(a, b));
  return 0;
}

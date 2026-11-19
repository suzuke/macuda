// A server that is KILLED, not one that half-closes, with SIGPIPE left at its default (step B, 2026-11-19).
// test_tg_dead shuts only the server's sending side and ignores SIGPIPE, as llama-server does. On the card, the
// server was killed under llama-simple, which leaves SIGPIPE alone: the next request's send() hit a closed peer, the
// kernel raised SIGPIPE, and the client died with exit 141 that same millisecond - before M2 could latch the loss or
// M3 start the bus-master clear, so the card went on faulting through the DART until cleared by hand 11 s later.
// What has to hold: the process survives the send, the read returns the sentinel, and the loss latches.
#include "tinynv_pci.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#pragma pack(push, 1)
typedef struct { uint8_t status; uint64_t resp0, resp1; } resp_t;
#pragma pack(pop)

int main(void) {
  signal(SIGPIPE, SIG_DFL);
  setenv("TINYNV_EMERGENCY_BM", "0", 1);   // no helper process from a test
  char dir[] = "/tmp/tgpipe.XXXXXX", path[128];
  if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
  snprintf(path, sizeof(path), "%s/tinygpu.sock", dir);
  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 1)) { perror("listen"); return 2; }

  tinynv_pci_t pci;
  if (tinynv_pci_open_tinygpu(path, &pci)) { fprintf(stderr, "open: %s\n", tinynv_last_error()); return 2; }
  int srv = accept(lfd, NULL, NULL);
  if (srv < 0) { perror("accept"); return 2; }
  resp_t rp = {.status = 0, .resp0 = 0x2504};
  if (write(srv, &rp, sizeof(rp)) != (ssize_t)sizeof(rp)) { perror("reply"); return 2; }
  CHECK(pci.cfg_read(&pci, 2, 2) == 0x2504, "a config read the server answered");

  close(srv);   // the server is killed: both directions go at once, and the next send meets a closed peer
  CHECK(pci.cfg_read(&pci, 4, 2) == 0xffffffffu, "a read through a killed server reads as the sentinel");
  CHECK(pci.transport_dead && pci.transport_dead(&pci), "the loss did not latch");
  pci.cfg_write(&pci, 4, 2, 0x0403);
  CHECK(pci.transport_dead && pci.transport_dead(&pci), "the loss did not stay latched");
  pci.close(&pci);
  close(lfd);
  unlink(path);
  rmdir(dir);
  printf("test_tg_sigpipe: %d checks run, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

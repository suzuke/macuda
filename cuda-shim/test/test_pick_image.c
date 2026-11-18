// Which device image a multi-architecture binary hands the driver (libtinycudart/fatbin.c), against the committed
// cubins: laid end to end as an ARCH=multi build embeds them, and wrapped in a fatbin as nvcc -gencode x2 emits one.
// No GPU, no driver. Usage: test_pick_image <dir with copy1d.sm_86.cubin and copy1d.sm_120.cubin>
#include "../libtinycudart/fatbin.c"
#include <stdio.h>
#include <stdlib.h>

int tinycudart_pick_image(const void *, size_t, unsigned, const void **, size_t *, unsigned *);
int tinycudart_extract_cubin(const void *, unsigned, const void **, size_t *, unsigned *);

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned char *slurp(const char *dir, const char *name, size_t *n) {
  char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
  FILE *f = fopen(p, "rb"); if (!f) { printf("cannot open %s\n", p); exit(2); }
  fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc(*n); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f); return b;
}
static void put16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

int main(int argc, char **argv) {
  if (argc != 2) { printf("usage: %s <cubin dir>\n", argv[0]); return 2; }
  size_t n86, n120; unsigned char *a = slurp(argv[1], "copy1d.sm_86.cubin", &n86), *b = slurp(argv[1], "copy1d.sm_120.cubin", &n120);
  const void *out; size_t len; unsigned imgs;

  // end to end, as the Makefile's CUBIN_ARCH=multi embeds the runtime's own kernels
  size_t nc = n86 + n120; unsigned char *cat = malloc(nc); memcpy(cat, a, n86); memcpy(cat + n86, b, n120);
  CHECK(tinycudart_pick_image(cat, nc, 86, &out, &len, &imgs) == 0 && out == cat && len == n86 && imgs == 2, "RTX 3060 (8.6) takes the sm_86 image, whole");
  CHECK(tinycudart_pick_image(cat, nc, 89, &out, &len, &imgs) == 0 && out == cat && len == n86, "Ada (8.9) takes the sm_86 image");
  CHECK(tinycudart_pick_image(cat, nc, 120, &out, &len, &imgs) == 0 && out == cat + n86 && len == n120, "Blackwell (12.0) takes the sm_120 image, whole");
  CHECK(tinycudart_pick_image(cat, nc, 0, &out, &len, &imgs) == -2 && imgs == 2, "an unknown card with two images refuses rather than guesses");
  CHECK(tinycudart_pick_image(cat, nc, 75, &out, &len, &imgs) == -2, "a card neither image runs on refuses");
  // the reverse order picks by fit, not position
  memcpy(cat, b, n120); memcpy(cat + n120, a, n86);
  CHECK(tinycudart_pick_image(cat, nc, 86, &out, &len, &imgs) == 0 && out == cat + n120 && len == n86, "order does not matter");

  // two images that both run on the card: the newest wins (an sm_80 copy of the sm_86 image, relabelled in e_flags)
  unsigned char *two = malloc(2 * n86); memcpy(two, a, n86); memcpy(two + n86, a, n86); two[0x31] = 80;
  CHECK(tinycudart_pick_image(two, 2 * n86, 86, &out, &len, &imgs) == 0 && out == two + n86 && imgs == 2, "sm_80 and sm_86 on an 8.6 card: sm_86");
  memcpy(two, a, n86); memcpy(two + n86, a, n86); two[n86 + 0x31] = 80;
  CHECK(tinycudart_pick_image(two, 2 * n86, 89, &out, &len, &imgs) == 0 && out == two, "the same, the other way round: sm_86");
  free(two);

  // a single image is handed over fit or not (the driver's upload check owns that refusal): today's builds unchanged
  CHECK(tinycudart_pick_image(b, n120, 86, &out, &len, &imgs) == 0 && out == b && len == n120 && imgs == 1, "one image, wrong card: still handed over");
  CHECK(tinycudart_pick_image(a, n86, 0, &out, &len, &imgs) == 0 && out == a, "one image, unknown card: handed over");

  // truncation: the second image cut short is not an image, and nothing is read past the end (ASan)
  CHECK(tinycudart_pick_image(cat, nc - 1, 86, &out, &len, &imgs) == 0 && imgs == 1 && len == n120, "a truncated tail image is dropped, the whole one survives");
  CHECK(tinycudart_pick_image(cat, 40, 86, &out, &len, &imgs) == -1 && imgs == 0, "a header-sized scrap is no image");

  // a fatbin as nvcc -gencode x2 lays it out: 16-byte header, then entries of {kind, _, header size, payload size, ...}
  size_t eh = 64, pad = 8, fs = 2 * eh + n86 + pad + n120, fl = 16 + fs;
  unsigned char *fat = calloc(1, fl), *e = fat + 16;
  put32(fat, 0xBA55ED50u); put16(fat + 4, 1); put16(fat + 6, 16); put64(fat + 8, fs);
  put16(e, 2); put32(e + 4, (uint32_t)eh); put64(e + 8, n86 + pad); put32(e + 28, 86); memcpy(e + eh, a, n86); e += eh + n86 + pad;
  put16(e, 2); put32(e + 4, (uint32_t)eh); put64(e + 8, n120); put32(e + 28, 120); memcpy(e + eh, b, n120);
  CHECK(tinycudart_pick_image(fat, fl, 86, &out, &len, &imgs) == 0 && out == fat + 16 + eh && len == n86 && imgs == 2, "fatbin: 8.6 takes entry one, without its padding");
  CHECK(tinycudart_pick_image(fat, fl, 120, &out, &len, &imgs) == 0 && out == fat + 16 + 2 * eh + n86 + pad && len == n120, "fatbin: 12.0 takes entry two");
  struct fat_wrapper w = {0x466243b1, 1, fat, NULL};
  CHECK(tinycudart_extract_cubin(&w, 120, &out, &len, &imgs) == 0 && len == n120 && imgs == 2, "through clang's wrapper, bounded by the fatbin header");
  CHECK(tinycudart_extract_cubin(&w, 0, &out, &len, &imgs) == -2, "through the wrapper, an unknown card refuses");
  // an entry claiming more payload than the fatbin holds ends the walk instead of reading past it
  put64(fat + 16 + 8, fs);
  CHECK(tinycudart_pick_image(fat, fl, 86, &out, &len, &imgs) == -1 && imgs == 0, "an oversized entry stops the walk");

  printf(fails ? "%d of %d checks failed\n" : "image selection: all %d checks passed\n", fails ? fails : checks, checks);
  free(a); free(b); free(cat); free(fat);
  return fails != 0;
}

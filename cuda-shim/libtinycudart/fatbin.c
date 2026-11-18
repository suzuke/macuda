// Extract the device cubin (ELF) from the fatbin wrapper that clang's host code passes to __cudaRegisterFatBinary.
#include <stdint.h>
#include <string.h>
#include <stddef.h>
// wrapper clang/nvcc emits: {int magic=0x466243b1; int version; const void* data; void* filename}
struct fat_wrapper { int magic; int version; const void* data; void* filename; };
// ONE IMAGE PER ARCHITECTURE. A fatbin built for several cards (tinycc with TINYCC_ARCH=sm_86,sm_120a) carries one ELF
// per architecture, and the runtime's own kernels in an ARCH=multi build are their per-architecture cubins laid end to
// end. Either way the card decides: the newest image whose code runs on it (arch_compat.h, the same rule the driver
// checks on upload). A blob with a single image hands it over as before, fit or not, and the driver's upload check
// refuses a mismatch with its own message - so a single-architecture build behaves exactly as it did.
#include "../libtinynv/src/arch_compat.h"
#define FATBIN_MAGIC 0xBA55ED50u
static uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const unsigned char *p) { uint16_t v; memcpy(&v, p, 2); return v; }
// The extent of the ELF at e, or 0 when it is not one or does not fit in avail bytes. cubins put the program headers
// after the section headers, so the end is whichever table ends later.
static size_t elf_extent(const unsigned char *e, size_t avail) {
  if (avail < 64 || memcmp(e, "\x7f" "ELF", 4) || e[4] != 2) return 0;
  uint64_t sh = rd64(e + 0x28) + (uint64_t)rd16(e + 0x3a) * rd16(e + 0x3c);
  uint64_t ph = rd64(e + 0x20) + (uint64_t)rd16(e + 0x36) * rd16(e + 0x38);
  uint64_t end = sh > ph ? sh : ph;
  return end >= 64 && end <= avail ? (size_t)end : 0;
}
// the same rule as the driver's reader (libtinynv/src/cubin.c): ABI version 7 images keep the arch in the low byte
static unsigned elf_sm(const unsigned char *e) { uint32_t f = rd32(e + 0x30); return e[7] == 0x33 ? (f & 0xff) : ((f >> 8) & 0xff); }

int tinycudart_pick_image(const void *blob, size_t len, unsigned cc, const void **out, size_t *out_len, unsigned *n_out) {
  const unsigned char *b = blob;
  struct { const unsigned char *p; size_t n; unsigned sm; } img[16];
  unsigned n = 0;
  if (len >= 16 && rd32(b) == FATBIN_MAGIC) {
    uint64_t hs = rd16(b + 6), fs = rd64(b + 8), o = hs, lim = hs + fs < len ? hs + fs : len;
    while (o + 16 <= lim && n < 16) {
      uint32_t ehs = rd32(b + o + 4); uint64_t sz = rd64(b + o + 8);
      if (ehs < 16 || o + ehs > lim || sz > lim - o - ehs) break;
      size_t x = elf_extent(b + o + ehs, (size_t)sz);   // a compressed or PTX entry is not an ELF and is passed over
      if (x) { img[n].p = b + o + ehs; img[n].n = x; img[n].sm = elf_sm(b + o + ehs); n++; }
      o += ehs + sz;
    }
  } else {
    size_t o = 0, x;
    while (n < 16 && o < len && (x = elf_extent(b + o, len - o))) { img[n].p = b + o; img[n].n = x; img[n].sm = elf_sm(b + o); n++; o += x; }
  }
  if (n_out) *n_out = n;
  if (!n) return -1;
  int best = n == 1 ? 0 : -1;
  for (unsigned i = 0; n > 1 && i < n; i++)
    if (tinynv_sm_fits(img[i].sm, cc) && (best < 0 || img[i].sm > img[best].sm)) best = (int)i;
  if (best < 0) return -2;   // several images and none for this card (or the card is unknown): never guess
  *out = img[best].p; *out_len = img[best].n;
  return 0;
}

// The device code inside the wrapper clang emits. A fatbin header bounds the walk; without one (nothing this tree
// builds), the first ELF in the first 8 MB, as before.
int tinycudart_extract_cubin(const void* fatCubin, unsigned cc, const void** out, size_t* out_len, unsigned *n_out) {
  const struct fat_wrapper* w = (const struct fat_wrapper*)fatCubin;
  const unsigned char* data = (const unsigned char*)(w && w->magic == 0x466243b1 ? w->data : fatCubin);
  if (rd32(data) == FATBIN_MAGIC) return tinycudart_pick_image(data, 16 + (size_t)rd64(data + 8), cc, out, out_len, n_out);
  for (size_t i = 0; i < (8u<<20); i++) {
    if (memcmp(data+i, "\x7f" "ELF", 4) == 0 && data[i+4]==2 /*ELFCLASS64*/) {
      const unsigned char* e = data + i;
      *out = e; *out_len = (size_t)(rd64(e + 0x28) + (uint64_t)rd16(e + 0x3a) * rd16(e + 0x3c));
      if (n_out) *n_out = 1;
      return 0;
    }
  }
  return -1;
}

// --- the names of the kernels a cubin carries: PROGBITS sections named .text.<kernel> (the same rule the driver's
// reader uses, so the two never disagree about what is a kernel). Needed because clang on macOS and nvcc on Linux
// mangle the same kernel differently, and the runtime has to find nvcc's name for clang's.
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
int tinycudart_cubin_kernels(const void *cubin, size_t len, char ***names_out) {
  const unsigned char *e = cubin;
  if (len < 64 || memcmp(e, "\x7f" "ELF", 4) || e[4] != 2) return -1;
  uint64_t shoff; uint16_t shentsize, shnum, shstrndx;
  memcpy(&shoff, e + 40, 8); memcpy(&shentsize, e + 58, 2); memcpy(&shnum, e + 60, 2); memcpy(&shstrndx, e + 62, 2);
  if (shoff + (uint64_t)shnum * shentsize > len) return -1;
  uint64_t stroff; memcpy(&stroff, e + shoff + (uint64_t)shstrndx * shentsize + 24, 8);
  char **names = calloc(shnum, sizeof(char *)); int n = 0;
  for (uint16_t i = 0; i < shnum; i++) {
    const unsigned char *sh = e + shoff + (uint64_t)i * shentsize;
    uint32_t name, type; memcpy(&name, sh, 4); memcpy(&type, sh + 4, 4);
    if (type != 1 /* SHT_PROGBITS */) continue;
    const char *s = (const char *)e + stroff + name;
    if (strncmp(s, ".text.", 6)) continue;
    names[n++] = strdup(s + 6);
  }
  *names_out = names; return n;
}

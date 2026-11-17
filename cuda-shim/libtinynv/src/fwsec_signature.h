#ifndef TINYNV_FWSEC_SIGNATURE_H
#define TINYNV_FWSEC_SIGNATURE_H
#include <stddef.h>
#include <stdint.h>

/* Pure part of NVIDIA's fuse-matched FWSEC signature selection. No hardware IO. */
static inline int tinynv_fwsec_signature(uint32_t fuse, uint16_t mask, unsigned count,
                                        size_t block_size, size_t block_offset, size_t rom_size,
                                        unsigned *version_out, unsigned *index_out) {
  unsigned version = 0;
  for (uint32_t bits = fuse; bits; bits >>= 1) version++;
  if (version >= 16 || !(mask & (1u << version))) return -1;
  unsigned index = 0;
  for (unsigned bit = 0; bit < version; bit++) index += (mask >> bit) & 1u;
  size_t end = ((size_t)index + 1) * 0x180u;
  if (index >= count || end > block_size || block_offset > rom_size || end > rom_size - block_offset)
    return -1;
  *version_out = version;
  *index_out = index;
  return 0;
}
#endif

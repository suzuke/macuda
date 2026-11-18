// Tensor-core speed unlock for GA10x (Ampere) via the V67 SEC2 exploit.
//
// The consumer RTX fuse bits throttle FP16/BF16/TF32 tensor throughput to half speed.
// This module fires the 610.43.03 booter_load on SEC2 with a crafted V67 payload as the
// WprMeta signature, overflowing the booter's DMEM stack and executing a ROP chain that
// writes FEAT_OVR_PLM to 0xFFFFFFFF at L3 privilege.  Once the PLM is open the host can
// write SS0/SS1 to override the fuse throttle.
//
// Behavioural port of nv-uefi-unlocker (GPL-2.0-only), adapted for macuda's boot flow.
#include "tensor_unlock.h"
#include "tensor_unlock_state.h"
#include "internal.h"
#include "flcn.h"
#include "gsp.h"
#include "nv_regs.h"
#include "nv_structs.h"
#include "fw.h"
#include "mmu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void tu_usleep(int us) {
  struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000L };
  nanosleep(&ts, NULL);
}

#define FLD(reg, f) ((uint32_t)(((1ull << (reg##_##f##_HI - reg##_##f##_LO + 1)) - 1) << reg##_##f##_LO))

// ---- FEAT / speed-select registers (undocumented, from nv-uefi-unlocker) ----
#define FEAT_OVR_PLM    0x00823804u
#define FEAT_READOUT_1  0x00823818u
#define FEAT_SS0        0x0082381cu
#define FEAT_SS1        0x00823820u
#define SS0_UNLOCK      0x88888888u
#define SS1_UNLOCK      0x00000008u

// ---- V67 payload (from nv-uefi-unlocker payload.rs PATCHED_WORDS_GA102, GPL-2.0) ----
#define V67_SIZE  0xFA00u

static const struct { uint32_t off, val; } v67_patches[] = {
  {0x1100, 0x00000007},
  {0xF948, 0xFFFFFFFF},
  {0xF950, 0x00000D44},
  {0xF960, 0x00823804},
  {0xF968, 0x00001FCE},
  {0xF974, 0x00000000},
  {0xF97C, 0x00001101},
  {0xF980, 0x000084C8},
  {0xF984, 0x00008E18},
  {0xF98C, 0x000084C8},
  {0xF990, 0x00000000},
  {0xF998, 0x00001FCE},
  {0xF9A4, 0x0000FFBC},
  {0xF9AC, 0x00005789},
  {0xF9BC, 0x00000D44},
  {0xF9CC, 0x00000003},
  {0xF9D4, 0x00001FCE},
  {0xF9E8, 0x00000D52},
  {0xF9EC, 0x000081EE},
};

static void build_v67(uint8_t *buf) {
  uint32_t fill = 0x0000019Cu;
  for (size_t i = 0; i < V67_SIZE; i += 4)
    memcpy(buf + i, &fill, 4);
  for (size_t i = 0; i < sizeof(v67_patches)/sizeof(v67_patches[0]); i++) {
    uint32_t v = v67_patches[i].val;
    memcpy(buf + v67_patches[i].off, &v, 4);
  }
}

// ---- BTL1 bundle format (72-byte header + image + signatures) ----
#define BTL1_MAGIC 0x314C5442u

typedef struct {
  uint32_t imem_ns_off, imem_ns_size;
  uint32_t data_off, data_size;
  uint32_t app_code_off, app_code_size;
  uint32_t patch_loc;
  uint32_t ucode_id, engine_id;
  uint32_t num_sigs, sig_bytes;
  uint32_t image_size, sig_total;
} btl1_hdr_t;

static uint32_t le32(const uint8_t *p, size_t off) {
  uint32_t v;
  memcpy(&v, p + off, 4);
  return v;
}

static int parse_btl1(const uint8_t *bundle, size_t bundle_len, btl1_hdr_t *h,
                      const uint8_t **image_out, const uint8_t **sigs_out) {
  if (bundle_len < 72 || le32(bundle, 0) != BTL1_MAGIC || le32(bundle, 4) != 1)
    return tinynv_fail("btl1: bad magic or version");
  #define H(i) le32(bundle, 8 + (i) * 4)
  h->imem_ns_off  = H(0);  h->imem_ns_size = H(1);
  h->data_off     = H(2);  h->data_size    = H(3);
  h->app_code_off = H(5);  h->app_code_size= H(6);
  h->patch_loc    = H(9);
  h->ucode_id     = H(10); h->engine_id    = H(11);
  h->num_sigs     = H(12); h->sig_bytes    = 0;
  h->image_size   = le32(bundle, 8 + 14 * 4);
  h->sig_total    = le32(bundle, 8 + 15 * 4);
  #undef H
  if (h->num_sigs && h->sig_total)
    h->sig_bytes = h->sig_total / h->num_sigs;
  fprintf(stderr, "libtinynv: tensor_unlock: btl1: imem_ns=%u+%u data=%u+%u app=%u+%u patch=%u ucode=%u engine=%u nsigs=%u\n",
          h->imem_ns_off, h->imem_ns_size, h->data_off, h->data_size,
          h->app_code_off, h->app_code_size, h->patch_loc, h->ucode_id, h->engine_id, h->num_sigs);
  if (h->image_size != 60416 || h->patch_loc != 35344 || h->ucode_id != 3 || h->engine_id != 1 || h->num_sigs != 2)
    return tinynv_fail("btl1: GA102 geometry mismatch (img %u patch %u ucode %u engine %u nsigs %u)",
                       h->image_size, h->patch_loc, h->ucode_id, h->engine_id, h->num_sigs);
  if (bundle_len != 72 + h->image_size + h->sig_total)
    return tinynv_fail("btl1: bundle size %zu, expected %u", bundle_len, 72 + h->image_size + h->sig_total);
  *image_out = bundle + 72;
  *sigs_out = bundle + 72 + h->image_size;
  return 0;
}

// Patch the fuse-selected stock signature into the booter image so the boot ROM accepts it.
static int patch_stock_sig(uint8_t *image, const btl1_hdr_t *h, const uint8_t *sigs, uint32_t fuse_ver) {
  if (h->num_sigs < 2 || h->sig_bytes != 384)
    return tinynv_fail("tensor_unlock: unexpected signature table (%u sigs, %u bytes each)", h->num_sigs, h->sig_bytes);
  uint32_t idx = h->num_sigs - 1 - fuse_ver;
  if (idx >= h->num_sigs)
    return tinynv_fail("tensor_unlock: fuse version %u out of range for %u signatures", fuse_ver, h->num_sigs);
  if ((size_t)h->patch_loc + h->sig_bytes > h->image_size)
    return tinynv_fail("tensor_unlock: signature patch outside image");
  memcpy(image + h->patch_loc, sigs + idx * h->sig_bytes, h->sig_bytes);
  return 0;
}

// Fire the 610.43.03 booter on SEC2 with the V67 payload as the WprMeta signature,
// then poll FEAT_OVR_PLM for the flip.  Returns 0 if PLM opened, -1 otherwise.
static int fire_v67(tinynv_gpu_t *g, const btl1_hdr_t *h, const uint8_t *patched_image,
                    tinynv_bootmem_t *v67_mem, tinynv_bootmem_t *meta_mem) {
  tinynv_dev_t *d = &g->dev;
  uint64_t sec2 = g->flcn.sec2;

  // allocate VRAM for the booter image
  tinynv_bootmem_t *img_mem = &g->tensor.image;
  if (tinynv_alloc_boot_mem(&g->mm, h->image_size, patched_image, 0, img_mem)) return -1;
  // From here an engine reset or DMA may be in flight. The caller must cold-reset
  // the device before freeing any of the three buffers, including on timeouts.
  g->tensor.pending = 1;

  // load the booter into SEC2 exactly as flcn_execute_hs does
  if (tinynv_flcn_reset(g, sec2, 0)) goto fail_img;
  tinynv_flcn_disable_ctx_req(d, sec2);

  // aperture 0 = VRAM, physical addressing
  uint32_t transcfg = tinynv_rd32(d, sec2 + NV_PFALCON_FBIF_TRANSCFG(0));
  transcfg &= ~(FLD(NV_PFALCON_FBIF_TRANSCFG, TARGET) | FLD(NV_PFALCON_FBIF_TRANSCFG, MEM_TYPE));
  transcfg |= NV_SET(NV_PFALCON_FBIF_TRANSCFG, TARGET, 0) |
              NV_SET(NV_PFALCON_FBIF_TRANSCFG, MEM_TYPE, NV_PFALCON_FBIF_TRANSCFG_MEM_TYPE_PHYSICAL);
  tinynv_wr32(d, sec2 + NV_PFALCON_FBIF_TRANSCFG(0), transcfg);

  // IMEM: secure code — mirrors flcn_dma(d, sec2, cmd, dest=0, mem_off=code_off, src=img_paddr, size=code_sz)
  // DMATRFBASE = image base address >> 8;  DMATRFFBOFFS = code_off + done  (source offset in VRAM from base)
  // DMATRFMOFFS = 0 + done  (destination in falcon IMEM)
  uint32_t cmd = NV_SET(NV_PFALCON_FALCON_DMATRFCMD, WRITE, 0) |
                 NV_SET(NV_PFALCON_FALCON_DMATRFCMD, SIZE, NV_PFALCON_FALCON_DMATRFCMD_SIZE_256B) |
                 NV_SET(NV_PFALCON_FALCON_DMATRFCMD, CTXDMA, 0) |
                 NV_SET(NV_PFALCON_FALCON_DMATRFCMD, IMEM, 1) | NV_SET(NV_PFALCON_FALCON_DMATRFCMD, SEC, 1);
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFBASE, (uint32_t)(img_mem->paddr >> 8));
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFBASE1, (uint32_t)((img_mem->paddr >> 8) >> 32) & 0x1ff);
  for (uint32_t done = 0; done < h->app_code_size; done += 256) {
    if (tinynv_wait_reg(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, FLD(NV_PFALCON_FALCON_DMATRFCMD, FULL), 0,
                        5000, "tensor_unlock: IMEM DMA queue")) goto fail_img;
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFMOFFS, done);
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFFBOFFS, h->app_code_off + done);
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, cmd);
  }
  if (tinynv_wait_reg(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, FLD(NV_PFALCON_FALCON_DMATRFCMD, IDLE),
                      FLD(NV_PFALCON_FALCON_DMATRFCMD, IDLE), 10000, "tensor_unlock: IMEM DMA")) goto fail_img;

  // DMEM: mirrors flcn_dma(d, sec2, cmd, dest=0, mem_off=0, src=img_paddr+data_off, size=data_sz)
  cmd = NV_SET(NV_PFALCON_FALCON_DMATRFCMD, WRITE, 0) |
        NV_SET(NV_PFALCON_FALCON_DMATRFCMD, SIZE, NV_PFALCON_FALCON_DMATRFCMD_SIZE_256B) |
        NV_SET(NV_PFALCON_FALCON_DMATRFCMD, CTXDMA, 0) |
        NV_SET(NV_PFALCON_FALCON_DMATRFCMD, IMEM, 0) | NV_SET(NV_PFALCON_FALCON_DMATRFCMD, SEC, 0);
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFBASE, (uint32_t)((img_mem->paddr + h->data_off) >> 8));
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFBASE1,
              (uint32_t)(((img_mem->paddr + h->data_off) >> 8) >> 32) & 0x1ff);
  for (uint32_t done = 0; done < h->data_size; done += 256) {
    if (tinynv_wait_reg(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, FLD(NV_PFALCON_FALCON_DMATRFCMD, FULL), 0,
                        5000, "tensor_unlock: DMEM DMA queue")) goto fail_img;
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFMOFFS, done);
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFFBOFFS, done);
    tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, cmd);
  }
  if (tinynv_wait_reg(d, sec2 + NV_PFALCON_FALCON_DMATRFCMD, FLD(NV_PFALCON_FALCON_DMATRFCMD, IDLE),
                      FLD(NV_PFALCON_FALCON_DMATRFCMD, IDLE), 10000, "tensor_unlock: DMEM DMA")) goto fail_img;

  // BROM params: PKC offset is where the stock sig sits in DMEM (patch_loc - data_off)
  uint32_t sig_dmem = h->patch_loc - h->data_off;
  tinynv_wr32(d, sec2 + NV_PFALCON2_FALCON_BROM_PARAADDR(0), sig_dmem);
  tinynv_wr32(d, sec2 + NV_PFALCON2_FALCON_BROM_ENGIDMASK, h->engine_id);
  tinynv_wr32(d, sec2 + NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID,
              NV_SET(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID, VAL, h->ucode_id));
  tinynv_wr32(d, sec2 + NV_PFALCON2_FALCON_MOD_SEL,
              NV_SET(NV_PFALCON2_FALCON_MOD_SEL, ALGO, NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K));

  // boot vector, mailbox (WprMeta address), start
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_BOOTVEC, h->app_code_off);
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_MAILBOX0, (uint32_t)meta_mem->addrs[0]);
  tinynv_wr32(d, sec2 + NV_PFALCON_FALCON_MAILBOX1, (uint32_t)(meta_mem->addrs[0] >> 32));

  fprintf(stderr, "libtinynv: tensor_unlock: firing 610.43.03 booter on sec2 (sig_dmem %#x, meta %#llx)\n",
          sig_dmem, (unsigned long long)meta_mem->addrs[0]);

  tinynv_flcn_start_cpu(d, sec2);

  // poll FEAT_OVR_PLM for flip (adapted from nv-uefi-unlocker BooterWait::PlmFlip)
  double start = tinynv_now_s(), deadline = start + 5.0;
  enum tinynv_tensor_poll_result poll = TINYNV_TENSOR_POLL_WAIT;
  for (;;) {
    uint32_t cpuctl = tinynv_rd32(d, sec2 + NV_PFALCON_FALCON_CPUCTL);
    uint32_t plm = tinynv_rd32(d, FEAT_OVR_PLM);
    uint32_t witness = tinynv_rd32(d, FEAT_SS0);
    poll = tinynv_tensor_poll_status(plm, witness,
        NV_GET(cpuctl, NV_PFALCON_FALCON_CPUCTL, HALTED), tinynv_now_s() >= deadline);
    if (poll == TINYNV_TENSOR_POLL_OPEN) {
      fprintf(stderr, "libtinynv: tensor_unlock: PLM opened after %.3f s\n", tinynv_now_s() - start);
      break;
    }
    if (poll != TINYNV_TENSOR_POLL_WAIT) break;
    tu_usleep(75);
  }

  if (poll == TINYNV_TENSOR_POLL_FEAT_UNREADABLE)
    return tinynv_fail("tensor_unlock: FEAT block unreadable while polling PLM");
  if (poll == TINYNV_TENSOR_POLL_HALTED) {
    uint32_t plm = tinynv_rd32(d, FEAT_OVR_PLM);
    uint32_t mbox0 = tinynv_rd32(d, sec2 + NV_PFALCON_FALCON_MAILBOX0);
    return tinynv_fail("tensor_unlock: booter halted without opening PLM (PLM %#x, mailbox %#x)", plm, mbox0);
  }
  if (poll == TINYNV_TENSOR_POLL_TIMEOUT)
    return tinynv_fail("tensor_unlock: timed out after 5 seconds waiting for PLM to open");
  return 0;

fail_img:
  return -1;
}

int tinynv_tensor_unlock(tinynv_gpu_t *g) {
  const char *env = getenv("TINYNV_TENSOR_UNLOCK");
  if (!env) return 0;

  tinynv_dev_t *d = &g->dev;
  if (d->architecture != 0x17) {
    fprintf(stderr, "libtinynv: tensor_unlock: skipped (arch %#x, only Ampere 0x17 supported)\n", d->architecture);
    return 0;
  }

  int want_lock = strcmp(env, "0") == 0;
  if (!want_lock && strcmp(env, "1") != 0) return 0;
  uint32_t plm = tinynv_rd32(d, FEAT_OVR_PLM);
  uint32_t ss0 = tinynv_rd32(d, FEAT_SS0);
  uint32_t ss1 = tinynv_rd32(d, FEAT_SS1);
  fprintf(stderr, "libtinynv: tensor_unlock: PLM=%#x SS0=%#x SS1=%#x\n", plm, ss0, ss1);

  // A PLM of all ones also denotes an inaccessible MMIO block. Require a
  // non-error read in the same block before interpreting that value as open.
  if (!tinynv_feat_block_responds(ss0) || !tinynv_feat_block_responds(ss1) || plm == TINYNV_FEAT_BADF ||
      !tinynv_feat_block_responds(tinynv_rd32(d, FEAT_READOUT_1)))
    return tinynv_fail("tensor_unlock: FEAT block is unreadable");
  if (g->tensor.verify_after_reset) {
    g->tensor.verify_after_reset = 0;
    if (ss0 != g->tensor.expected_ss0 || ss1 != g->tensor.expected_ss1)
      return tinynv_fail("tensor_unlock: selectors changed across reset (SS0 %#x SS1 %#x)", ss0, ss1);
  }
  if (want_lock && !(ss0 & 0x88888888u) && !(ss1 & 8u)) {
    fprintf(stderr, "libtinynv: tensor_unlock: selector override enable bits already clear\n");
    return 0;
  }
  if (!want_lock && ss0 == SS0_UNLOCK && ss1 == SS1_UNLOCK) {
    fprintf(stderr, "libtinynv: tensor_unlock: speed-select overrides already present\n");
    return 0;
  }
  uint32_t target0 = want_lock ? ss0 & 0x77777777u : SS0_UNLOCK;
  uint32_t target1 = want_lock ? ss1 & ~8u : SS1_UNLOCK;

  int did_exploit = 0;
  if (plm == 0xFFFFFFFFu) {
    fprintf(stderr, "libtinynv: tensor_unlock: PLM already open, skipping exploit fire\n");
    goto write_selectors;
  }

  {
    // load 610.43.03 BTL1 booter
    tinynv_blob_t btl1_fw;
    if (tinynv_fw_load("ga102", "booter_load_v67-610.43.03.btl1",
                       "056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4", &btl1_fw))
      return tinynv_fail("tensor_unlock: cannot load 610.43.03 booter (run tools/fetch_610_booter.py)");

    btl1_hdr_t hdr;
    const uint8_t *raw_image, *sigs;
    if (parse_btl1(btl1_fw.data, btl1_fw.size, &hdr, &raw_image, &sigs)) {
      tinynv_fw_free(&btl1_fw);
      return -1;
    }

    // read fuse version for signature selection (bit-length of fuse value, matching fwsec_signature.h)
    uint32_t fuse_raw = tinynv_rd32(d, 0x00824140u + (hdr.ucode_id - 1) * 4);
    uint32_t fuse_ver = 0;
    for (uint32_t bits = fuse_raw; bits; bits >>= 1) fuse_ver++;
    fprintf(stderr, "libtinynv: tensor_unlock: fuse_raw %#x, fuse_ver %u, sig_idx %u/%u\n",
            fuse_raw, fuse_ver, fuse_ver < hdr.num_sigs ? hdr.num_sigs - 1 - fuse_ver : 0, hdr.num_sigs);

    // make a mutable copy and patch the stock signature (boot ROM needs it)
    uint8_t *image = malloc(hdr.image_size);
    if (!image) { tinynv_fw_free(&btl1_fw); return tinynv_fail("tensor_unlock: no memory for booter image"); }
    memcpy(image, raw_image, hdr.image_size);
    if (patch_stock_sig(image, &hdr, sigs, fuse_ver)) {
      free(image); tinynv_fw_free(&btl1_fw);
      return -1;
    }

    // build V67 payload and allocate DMA memory
    uint8_t *v67 = malloc(V67_SIZE);
    if (!v67) { free(image); tinynv_fw_free(&btl1_fw); return tinynv_fail("tensor_unlock: no memory for V67"); }
    build_v67(v67);

    tinynv_bootmem_t *v67_mem = &g->tensor.payload, *meta_mem = &g->tensor.meta;
    if (tinynv_alloc_boot_mem(&g->mm, V67_SIZE, v67, -1, v67_mem)) {
      tinynv_tensor_cleanup(g);
      free(v67); free(image); tinynv_fw_free(&btl1_fw);
      return -1;
    }
    free(v67);

    // build a modified WprMeta: copy macuda's existing meta, replace signature with V67
    tinynv_wpr_meta_t meta;
    if (g->gsp.wpr_meta.size < sizeof(meta)) {
      tinynv_tensor_cleanup(g);
      free(image); tinynv_fw_free(&btl1_fw);
      return tinynv_fail("tensor_unlock: existing WprMeta too small");
    }
    nv_rd_block(&g->gsp.wpr_meta.view, 0, &meta, sizeof(meta));
    meta.sysmemAddrOfSignature = v67_mem->addrs[0];
    meta.sizeOfSignature = V67_SIZE;

    if (tinynv_alloc_boot_mem(&g->mm, sizeof(meta), &meta, -1, meta_mem)) {
      tinynv_tensor_cleanup(g);
      free(image); tinynv_fw_free(&btl1_fw);
      return -1;
    }

    int rc = fire_v67(g, &hdr, image, v67_mem, meta_mem);

    if (!g->tensor.pending) tinynv_tensor_cleanup(g);
    free(image);
    tinynv_fw_free(&btl1_fw);

    if (rc) return -1;
    did_exploit = 1;
  }

write_selectors:
  tinynv_wr32(d, FEAT_SS0, target0);
  tinynv_wr32(d, FEAT_SS1, target1);
  uint32_t ss0_rb = tinynv_rd32(d, FEAT_SS0), ss1_rb = tinynv_rd32(d, FEAT_SS1);
  uint32_t readout = tinynv_rd32(d, FEAT_READOUT_1);
  fprintf(stderr, "libtinynv: tensor_unlock: SS0=%#x SS1=%#x readout=%#x\n", ss0_rb, ss1_rb, readout);
  int selectors_ok = tinynv_tensor_selectors_match(target0, target1, ss0_rb, ss1_rb, readout);
  // Restore the observed gate where possible. GA106 has been observed to retain
  // bit 6 (8f -> ef); accept that known closed readback but never all-ones.
  if (did_exploit || plm != 0xffffffffu) tinynv_wr32(d, FEAT_OVR_PLM, plm);
  uint32_t plm_rb = tinynv_rd32(d, FEAT_OVR_PLM);
  ss0_rb = tinynv_rd32(d, FEAT_SS0); ss1_rb = tinynv_rd32(d, FEAT_SS1);
  int plm_ok = tinynv_tensor_plm_restore_matches(plm, plm_rb);
  if (!selectors_ok || ss0_rb != target0 || ss1_rb != target1 || !plm_ok) {
    int rollback_ok = 0;
    if (tinynv_feat_block_responds(ss0_rb) && tinynv_feat_block_responds(ss1_rb) &&
        tinynv_feat_block_responds(tinynv_rd32(d, FEAT_READOUT_1))) {
      tinynv_wr32(d, FEAT_SS0, ss0);
      tinynv_wr32(d, FEAT_SS1, ss1);
      rollback_ok = tinynv_rd32(d, FEAT_SS0) == ss0 && tinynv_rd32(d, FEAT_SS1) == ss1;
    }
    return tinynv_fail("tensor_unlock: selector/PLM verification failed (SS0 %#x SS1 %#x PLM %#x; "
                       "expected %#x/%#x/%#x, rollback %s)",
                       ss0_rb, ss1_rb, plm_rb, target0, target1, plm, rollback_ok ? "verified" : "unverified");
  }
  if (!did_exploit) return 0;
  g->tensor.expected_ss0 = target0; g->tensor.expected_ss1 = target1;
  g->tensor.verify_after_reset = 1;
  g->tensor.retry = 1;
  return tinynv_fail("tensor_unlock: selector change requires cold reset and fresh firmware boot");
}

void tinynv_tensor_cleanup(tinynv_gpu_t *g) {
  if (g->tensor.image.size) tinynv_free_boot_mem(&g->mm, &g->tensor.image);
  if (g->tensor.payload.size) tinynv_free_boot_mem(&g->mm, &g->tensor.payload);
  if (g->tensor.meta.size) tinynv_free_boot_mem(&g->mm, &g->tensor.meta);
  g->tensor.pending = 0;
  g->tensor.retry = 0;
}

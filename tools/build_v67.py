#!/usr/bin/env python3
"""Build the V67 payload for GA102 (Ampere) and verify against nv-uefi-unlocker's pinned SHA-256.

GPL-2.0-only behavioral port of nv-uefi-unlocker/assets/src/payload.rs PATCHED_WORDS_GA102.
The payload exploits the SEC2 booter's signature-read stack overflow to write FEAT_OVR_PLM=0xFFFFFFFF
at L3 privilege, opening the speed-select override registers for host MMIO.
"""
import hashlib, struct, sys, os

V67_SIZE = 0xFA00
FILL_DWORD = 0x0000019C  # self-loop instruction (observable stop)

# (byte_offset, little-endian u32 value) — from nv-uefi-unlocker payload.rs PATCHED_WORDS_GA102
PATCHES_GA102 = [
    (0x1100, 0x00000007),
    (0xF948, 0xFFFFFFFF),  # write value: PLM open
    (0xF950, 0x00000D44),
    (0xF960, 0x00823804),  # target: FEAT_OVR_PLM
    (0xF968, 0x00001FCE),
    (0xF974, 0x00000000),
    (0xF97C, 0x00001101),
    (0xF980, 0x000084C8),
    (0xF984, 0x00008E18),
    (0xF98C, 0x000084C8),
    (0xF990, 0x00000000),
    (0xF998, 0x00001FCE),
    (0xF9A4, 0x0000FFBC),
    (0xF9AC, 0x00005789),
    (0xF9BC, 0x00000D44),
    (0xF9CC, 0x00000003),
    (0xF9D4, 0x00001FCE),
    (0xF9E8, 0x00000D52),
    (0xF9EC, 0x000081EE),  # terminator
]

V67_PAYLOAD_SHA256 = "2a0aa1f86d0b6afff5214d641d87d46dc98ef06ed908ff05d0ddbad5ffb7e197"

def build_v67(fill, size, patches):
    buf = bytearray(size)
    fill_le = struct.pack('<I', fill)
    for i in range(0, size, 4):
        buf[i:i+4] = fill_le
    for off, val in patches:
        assert off + 4 <= size, f"patch at {off:#x} outside payload"
        struct.pack_into('<I', buf, off, val)
    return bytes(buf)

def main():
    payload = build_v67(FILL_DWORD, V67_SIZE, PATCHES_GA102)
    digest = hashlib.sha256(payload).hexdigest()
    ok = digest == V67_PAYLOAD_SHA256
    print(f"V67 GA102 payload: {len(payload)} bytes")
    print(f"  SHA-256: {digest}")
    print(f"  pinned:  {V67_PAYLOAD_SHA256}")
    print(f"  match:   {'YES' if ok else 'NO — MISMATCH'}")
    if not ok:
        sys.exit(1)
    if len(sys.argv) > 1:
        out = sys.argv[1]
        with open(out, 'wb') as f:
            f.write(payload)
        print(f"  wrote:   {out}")

if __name__ == '__main__':
    main()

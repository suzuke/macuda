#!/usr/bin/env python3
"""Fetch the GA102 booter_load ucode from the NVIDIA 610.43.03 kernel module
source archive and build the BTL1 bundle.

Run as: python3 -I tools/fetch_610_booter.py
(The archive is untrusted input; -I isolates the interpreter from the
environment and the current directory.)
"""
import hashlib
import os
import re
import struct
import sys
import tarfile
import urllib.request
import zlib

VER = "610.43.03"
URL = f"https://download.nvidia.com/XFree86/NVIDIA-kernel-module-source/NVIDIA-kernel-module-source-{VER}.tar.xz"
ARCHIVE_SHA = "7e118923c7a23edc36114d63273a46e3e04e9af98695a42203e7ac2dfe9fc1dc"
MEMBER = (f"NVIDIA-kernel-module-source-{VER}/src/nvidia/generated/"
          "g_bindata_kgspGetBinArchiveBooterLoadUcode_GA102.c")
BINDATA_SHA = "04cd3db7f670adc4efd79d2e76217bb809fffddfbd6f1af884cde2af26dd39af"
BUNDLE_SHA = "056cbc064a888f80717726c54d7f64fcd68044d123870c95a74f37f4975dcaa4"
CACHE_DIR = "/private/tmp/claude-501/nv-uefi-unlocker-cache"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(
    SCRIPT_DIR, "..", "cuda-shim", "libtinynv", "third_party", "firmware",
    "nvidia", "ga102", "gsp", f"booter_load_v67-{VER}.btl1"))
MAX_MEMBER = 64 * 1024 * 1024
LABELS = ["IMAGE_PROD", "HEADER_PROD", "SIG_PROD", "PATCH_LOC", "PATCH_META", "NUM_SIGS"]
SIG_SIZE = 384


def die(msg):
    sys.exit(f"ERROR: {msg}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def get_archive():
    os.makedirs(CACHE_DIR, exist_ok=True)
    path = os.path.join(CACHE_DIR, os.path.basename(URL))
    if os.path.exists(path) and sha256_file(path) == ARCHIVE_SHA:
        print(f"archive: cached {path}")
        return path
    print(f"archive: downloading {URL}")
    tmp = path + ".part"
    with urllib.request.urlopen(URL, timeout=60) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    got = sha256_file(tmp)
    if got != ARCHIVE_SHA:
        os.remove(tmp)
        die(f"archive sha256 mismatch: {got}")
    os.replace(tmp, path)
    return path


def read_member(archive):
    with tarfile.open(archive, "r:xz") as tf:
        m = tf.getmember(MEMBER)
        if not m.isfile() or m.size > MAX_MEMBER:
            die("unexpected archive member type/size")
        data = tf.extractfile(m).read()
    got = hashlib.sha256(data).hexdigest()
    if got != BINDATA_SHA:
        die(f"BINDATA sha256 mismatch: {got}")
    return data.decode("ascii")


def parse_label(text, label):
    pat = re.compile(
        r"((?://[^\n]*\n)+)static\s[^\n=]*?BINDATA_LABEL_" + label +
        r"_data\[\]\s*=\s*\{(.*?)\};", re.S)
    m = pat.search(text)
    if not m:
        die(f"label {label} not found")
    meta = m.group(1)
    comp = re.findall(r"COMPRESSION:\s*(YES|NO)", meta)
    size = re.findall(r"DATA SIZE \(bytes\):\s*(\d+)", meta)
    if not comp or not size:
        die(f"{label}: missing metadata")
    comp, size = comp[-1] == "YES", int(size[-1])
    raw = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{1,2})", m.group(2)))
    if comp:
        csize = re.findall(r"COMPRESSED SIZE \(bytes\):\s*(\d+)", meta)
        if csize and int(csize[-1]) != len(raw):
            die(f"{label}: compressed size mismatch")
        if len(raw) < 8:
            die(f"{label}: compressed blob too short")
        body, trailer = raw[:-8], raw[-8:]
        crc, isize = struct.unpack("<II", trailer)
        d = zlib.decompressobj(-15)
        out = d.decompress(body, size + 1)
        if not d.eof:
            die(f"{label}: deflate stream not terminated within declared size")
        if len(out) != size or isize != size:
            die(f"{label}: size mismatch (got {len(out)}, isize {isize}, declared {size})")
        if zlib.crc32(out) & 0xFFFFFFFF != crc:
            die(f"{label}: CRC32 mismatch")
        return out
    if len(raw) != size:
        die(f"{label}: size mismatch (got {len(raw)}, declared {size})")
    return raw


def build_bundle(blobs):
    image, hdr, sig = blobs["IMAGE_PROD"], blobs["HEADER_PROD"], blobs["SIG_PROD"]
    if len(hdr) != 36:
        die("HEADER_PROD must be 36 bytes")
    if len(blobs["PATCH_LOC"]) != 4:
        die("PATCH_LOC must be 4 bytes")
    if len(blobs["PATCH_META"]) != 12:
        die("PATCH_META must be 12 bytes")
    if len(blobs["NUM_SIGS"]) != 4:
        die("NUM_SIGS must be 4 bytes")
    f = struct.unpack("<9I", hdr)
    patch_loc = struct.unpack("<I", blobs["PATCH_LOC"])[0]
    meta = struct.unpack("<3I", blobs["PATCH_META"])
    num_sigs = struct.unpack("<I", blobs["NUM_SIGS"])[0]

    if f[4] != 1:
        die(f"fields[4] must be 1, got {f[4]}")
    if num_sigs == 0 or len(sig) != num_sigs * SIG_SIZE:
        die("bad num_sigs / SIG_PROD length")
    if not (patch_loc >= f[2] and patch_loc + SIG_SIZE <= f[2] + f[3] <= len(image)):
        die("signature patch does not fit")
    for off, ln in ((f[0], f[1]), (f[2], f[3]), (f[5], f[6])):
        if off % 256 or ln % 256 or off + ln > len(image):
            die(f"transfer range ({off},{ln}) invalid")

    hdr14 = list(f) + [patch_loc, meta[2], meta[1], num_sigs, len(sig)]
    assert len(hdr14) == 14
    out = struct.pack("<2I", 0x314C5442, 1) + struct.pack("<14I", *hdr14)
    out += struct.pack("<2I", len(image), len(sig)) + image + sig
    # header layout check: 8 + 56 = 64, then image_size/sig_total at 64..72
    pins = {"image_size": (len(image), 60416), "patch_loc": (patch_loc, 35344),
            "ucode_id": (meta[2], 3), "engine_id": (meta[1], 1), "num_sigs": (num_sigs, 2)}
    for k, (got, want) in pins.items():
        if got != want:
            die(f"pin {k}: got {got}, expected {want}")
    return out


def main():
    text = read_member(get_archive())
    blobs = {l: parse_label(text, l) for l in LABELS}
    for l in LABELS:
        print(f"{l}: {len(blobs[l])} bytes")
    bundle = build_bundle(blobs)
    sha = hashlib.sha256(bundle).hexdigest()
    print(f"bundle: {len(bundle)} bytes")
    print(f"sha256: {sha}")
    if sha != BUNDLE_SHA:
        die(f"bundle sha256 mismatch, expected {BUNDLE_SHA}")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as fh:
        fh.write(bundle)
    print(f"saved: {OUT}")
    print("verification: OK (archive, BINDATA, CRC32/size, pins, bundle sha256)")


if __name__ == "__main__":
    main()

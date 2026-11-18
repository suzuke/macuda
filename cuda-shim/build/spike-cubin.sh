#!/bin/sh
# spike-cubin.sh <kernel.cu> <sm_XX> <out.cubin> — compile one spike kernel to a single-architecture cubin.
#
# The offline suite (libtinynv `make test`) reads spike/vecadd.sm{86,90,120}.cubin as fixtures: cubin parsing, the
# parameter base per architecture (test_api), the long-name cubin and the reference QMD. They are build output, so
# .gitignore keeps them out of the tree, and the only producer used to be libtinynv/tools/build_spike_cubin.py, which
# needs a tinygrad checkout and its compile server. This takes the same nvcc route as build/tinycc instead - a CUDA
# container on this Mac (build/container-runtime.sh picks Apple's `container` or Docker), or TINYCC_HOST over ssh -
# so a machine that can build ggml-cuda can build these too. No GPU is involved: -cubin compiles device code only.
set -eu
src=${1:?usage: spike-cubin.sh <kernel.cu> <sm_XX> <out.cubin>}; arch=${2:?arch, e.g. sm_86}; out=${3:?output path}
[ -f "$src" ] || { echo "spike-cubin: $src does not exist" >&2; exit 2; }
dir=$(cd "$(dirname "$src")" && pwd); base=$(basename "$src"); tmp="$out.tmp.$$"
trap 'rm -f "$tmp"' EXIT
CUDA_IMAGE=${TINYCC_CUDA_IMAGE:-nvidia/cuda:13.0.3-devel-ubuntu24.04}
NVCC="nvcc -cubin -arch=$arch"
if [ -n "${TINYCC_HOST:-}" ]; then
  # the kernel's directory travels with it: vecadd.cu includes tinycudart_compat.h from beside itself
  tar -C "$dir" -cf - . | ssh ${TINYCC_KEY:+-i "$TINYCC_KEY"} -o BatchMode=yes "$TINYCC_HOST" \
    "d=\$(mktemp -d) && tar -C \$d -xf - && export PATH=/usr/local/cuda/bin:\$PATH && $NVCC -I\$d -o \$d/out.cubin \$d/$base >&2 && cat \$d/out.cubin; rc=\$?; rm -rf \$d; exit \$rc" > "$tmp"
else
  RT=$(sh "$(dirname "$0")/container-runtime.sh")
  odir=$(cd "$(dirname "$out")" && pwd); oname=$(basename "$tmp")
  # --network=none and a read-only source mount, as in tinycc: a device compile has no business reaching anything else
  $RT run --rm --network=none --platform linux/arm64 --user "$(id -u):$(id -g)" -v "$dir:/s:ro" -v "$odir:/o" "$CUDA_IMAGE" \
    $NVCC -I/s -o "/o/$oname" "/s/$base" 2>&1 | grep -v -E 'NVIDIA Container Toolkit|docs.nvidia.com/datacenter|^\s*$|^=+$|^== CUDA ==|CUDA Version|Container image Copyright|This container image|By pulling|governed by|https://developer.nvidia.com/ngc|A copy of this license|WARNING: The NVIDIA Driver was not detected|^\*+|^tr: misaligned' >&2 || true   # the last: the image's own shell setup under --user, harmless
fi
# an ELF and nothing else, or it is not written: a truncated or empty cubin would fail later as a parse error in a test
head -c 4 "$tmp" 2>/dev/null | od -An -c | tr -d ' \n' | grep -q '177ELF' || { echo "spike-cubin: $arch compile of $src produced no ELF" >&2; exit 1; }
mv "$tmp" "$out"; trap - EXIT
echo "spike-cubin: $out ($(wc -c < "$out" | tr -d ' ') bytes, $arch)"

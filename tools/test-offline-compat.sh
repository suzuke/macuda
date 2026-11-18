#!/bin/sh
# Single small C compilation; no CUDA container, GPU access, or driver initialization.
set -eu
R=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$R/cuda-shim/libtinynv/test/test_fwsec_signature.c" -o "$work/test-fwsec"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$R/cuda-shim/libtinynv/test/test_arch_compat.c" -o "$work/test-arch"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  "$R/cuda-shim/test/test_pick_image.c" -o "$work/test-pick"
"$work/test-arch"
"$work/test-pick" "$R/cuda-shim/libtinycudart"
"$work/test-fwsec"
python3 "$R/tools/test-pci-preflight.py"

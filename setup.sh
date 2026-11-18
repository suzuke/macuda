#!/bin/sh
# setup.sh — fetch and build everything the shim needs that is not in this repository, in order.
#   sh setup.sh            every step below
#   sh setup.sh deps       NVIDIA open-gpu-kernel-modules headers + GSP firmware (pinned, hash-checked) + CUDA headers
#   sh setup.sh llama      clone llama.cpp at the pinned commit and build its CPU-only static tree (llama.cpp/build-null)
#   sh setup.sh sd         clone stable-diffusion.cpp at the pinned commit, point it at llama.cpp's ggml, apply the patch, build
#   sh setup.sh shim       make libtinynv.a + libtinycudart.a + libtinycublas.a (no GPU needed)
#   sh setup.sh cuda       compile ggml's CUDA backend: device code through nvcc, host code through clang
#   sh setup.sh link       link the *-null binaries against the shim (needs cuda-shim/build/libggml-cuda.a from the step above)
#   sh setup.sh split      Metal + CUDA in ONE process: a static Metal llama.cpp tree (llama.cpp/build-metal-null) linked
#                          against the same shim and archive -> build/bin/*-mc (split a model across the eGPU and the Mac GPU)
# Environment: LLAMA_SRC / SD_SRC name a local clone to copy from instead of GitHub; JOBS caps the parallel build (default:
# all cores). The device compile needs nvcc, which is Linux-only: set TINYCC_HOST / TINYCC_KEY to use a Linux box over ssh,
# or leave them unset and it runs in a CUDA container on this Mac (see cuda-shim/build/tinycc, and install.sh).
# PORTABLE BINARIES for another Mac: GGML_NATIVE=OFF GGML_CPU_ARM_ARCH=armv8.4-a+dotprod (every Apple Silicon chip; native
# on an M4 adds i8mm and SME, which an M1 does not have) LLAMA_OPENSSL=OFF (no Homebrew OpenSSL dylib to carry along)
# MACOSX_DEPLOYMENT_TARGET=14.0 (the default is this Mac's macOS). Re-run llama, then link, after changing them.
# ARCH picks the card the shim, CUDA and link steps build for: sm_120a (the default: Blackwell, the RTX 50 series) keeps
# today's layout exactly - build/shim, build/bin, libggml-cuda.a. Any other (sm_89 for Ada, the RTX 40 series; sm_86 for
# Ampere) builds beside it, never over it: build/shim-<arch>, libggml-cuda.<arch>.a, build/bin-<arch>, with that
# family's committed copy and GEMM kernels inside. Run those binaries with BIN=cuda-shim/build/bin-<arch> (tools/
# nv_shim_step.sh) or BIN=cuda-shim/build/bin-<arch>/llama-server-null (tools/serve.sh).
#   ARCH=sm_89 sh setup.sh shim && ARCH=sm_89 sh setup.sh cuda && ARCH=sm_89 sh setup.sh link
set -eu
R=$(cd "$(dirname "$0")" && pwd); cd "$R"
LLAMA_URL=https://github.com/ggml-org/llama.cpp.git
# 401563f10 (2026-11-17). Before it the pin was ad6c668 + a cherry-pick of 2f53959 (#28882, the ggml-cpu rope work-buffer
# fix test-backend-ops needs on arm64); 2f53959 is an ancestor of 401563f10, so nothing is applied on top any more.
# The README's numbers up to 2026-11-18 were measured on ad6c668+2f53959 (fb7e281b2 in an existing checkout).
# GGML_BACKEND_API_VERSION went 2 -> 3 between the two: the CMake trees, the registry objects and libggml-cuda.*.a must
# all come from this commit (re-run llama, cuda, link and split after moving it).
LLAMA_BASE=401563f10ec60754e688cd3165c1343cf4be9642
SD_URL=https://github.com/leejet/stable-diffusion.cpp.git
SD_COMMIT=59c23bce0d82be3a922023ab811194f05b3e2faa      # 59c23bc
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
# Unset, ARCH comes from the connected card's PCI device id (tools/pci-preflight.py --arch; read-only, from IOKit), so
# a 3060 builds sm_86 without being told. No card, several cards or an unknown one fall back to the default, sm_120a,
# and say so. A set ARCH always wins. Messages go to stderr: `sh setup.sh arch` prints the chosen ARCH alone.
if [ -z "${ARCH:-}" ]; then
  if ARCH=$(ioreg -a -r -c IOPCIDevice 2>/dev/null | python3 "$R/tools/pci-preflight.py" --arch 2>/dev/null) && [ -n "$ARCH" ]; then
    echo "ARCH=$ARCH: detected from the connected NVIDIA card (set ARCH to override)" >&2
  else
    why=$(ioreg -a -r -c IOPCIDevice 2>/dev/null | python3 "$R/tools/pci-preflight.py" --arch 2>&1 >/dev/null) || true
    ARCH=sm_120a
    echo "ARCH not set and no recognised card to detect it from (${why:-no answer}): building the default, sm_120a (Blackwell); set ARCH=sm_86 or sm_89 for an RTX 30 or 40" >&2
  fi
fi
case "$ARCH" in
  sm_120a) SUF=""; CUBIN_ARCH="" ;;                          # the tree's own default: nothing moves
  sm_8[0-9]) SUF="-$ARCH"; CUBIN_ARCH=sm_86 ;;               # Ampere and Ada: sm_89 runs the sm_86 kernels as they are
  sm_12[0-9]*) SUF="-$ARCH"; CUBIN_ARCH=sm_120 ;;
  multi) SUF="-multi"; CUBIN_ARCH=multi ;;                  # one binary for both families: sm_86 and sm_120a code, chosen per card at run time
  *) echo "ARCH=$ARCH: no committed copy/GEMM kernels for that family (have sm_86 and sm_120; ARCH=multi builds both)" >&2; exit 2 ;;
esac
export ARCH
step=${1:-all}

deps() {
  sh cuda-shim/libtinynv/tools/fetch_nv_headers.sh
  sh cuda-shim/libtinynv/tools/fetch_firmware.sh
  sh cuda-shim/build/fetch-cuda-headers.sh
}
clone_at() { # clone_at <dir> <url> <commit> [local source]
  d=$1; url=$2; c=$3; src=${4:-}
  if [ ! -d "$d/.git" ]; then
    if [ -n "$src" ]; then git clone -q "$src" "$d" && git -C "$d" remote set-url origin "$url"; else git clone -q "$url" "$d"; fi
  fi
  git -C "$d" cat-file -e "$c^{commit}" 2>/dev/null || git -C "$d" fetch -q origin "$c"
  git -C "$d" checkout -q --detach "$c"
}
llama() {
  # already at the pin? then leave the tree alone (a worktree counts: its .git is a file, not a directory)
  if [ "$(git -C llama.cpp rev-parse HEAD 2>/dev/null)" != "$LLAMA_BASE" ]; then
    clone_at llama.cpp "$LLAMA_URL" "$LLAMA_BASE" "${LLAMA_SRC:-}"
  fi
  echo "llama.cpp at $(git -C llama.cpp rev-parse --short HEAD): $(git -C llama.cpp log -1 --format=%s | cut -c1-70)"
  # CPU-only, static, no Metal: the shim replaces the CUDA backend at link time, and every other backend must be absent
  mkdir -p llama.cpp/build-null
  cmake -S llama.cpp -B llama.cpp/build-null -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_ACCELERATE=ON -DGGML_NATIVE=${GGML_NATIVE:-ON} ${GGML_CPU_ARM_ARCH:+-DGGML_CPU_ARM_ARCH=$GGML_CPU_ARM_ARCH} -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_TESTS=ON -DLLAMA_BUILD_SERVER=ON -DLLAMA_OPENSSL=${LLAMA_OPENSSL:-ON} > llama.cpp/build-null/cmake.log 2>&1 || { tail -20 llama.cpp/build-null/cmake.log; exit 1; }
  cmake --build llama.cpp/build-null -j"$JOBS" --target test-backend-ops llama-bench llama-simple llama-speculative-simple llama-server llama-mtmd-cli \
    > llama.cpp/build-null/build.log 2>&1 || { tail -30 llama.cpp/build-null/build.log; exit 1; }
  echo "llama.cpp/build-null built"
}
sd() {
  clone_at stable-diffusion.cpp "$SD_URL" "$SD_COMMIT" "${SD_SRC:-}"
  # upstream ggml (llama.cpp's), not leejet's fork: the shim's ggml-cuda archive is built from llama.cpp's tree
  if [ ! -L stable-diffusion.cpp/ggml ]; then rm -rf stable-diffusion.cpp/ggml; ln -s ../llama.cpp/ggml stable-diffusion.cpp/ggml; fi
  # Three states, told apart: applies (apply it), applies in reverse (already in), neither. The third used to print a
  # line and carry on, so a checkout holding an OLDER version of this patch built against a ggml it no longer matches.
  P=../patches/stable-diffusion.cpp-upstream-ggml.patch
  if git -C stable-diffusion.cpp apply --check "$P" 2>/dev/null; then
    git -C stable-diffusion.cpp apply "$P" && echo "patch applied"
  elif git -C stable-diffusion.cpp apply --check --reverse "$P" 2>/dev/null; then
    echo "patch already applied"
  else
    echo "stable-diffusion.cpp: $P neither applies nor is already applied - the checkout holds other edits (an older" >&2
    echo "  version of this patch, or local changes). Look at them with: git -C stable-diffusion.cpp diff -- src" >&2
    echo "  and, if nothing there is yours to keep: git -C stable-diffusion.cpp checkout -- src && sh setup.sh sd" >&2
    exit 1
  fi
  mkdir -p stable-diffusion.cpp/build-null
  cmake -S stable-diffusion.cpp -B stable-diffusion.cpp/build-null -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=OFF -DGGML_NATIVE=${GGML_NATIVE:-ON} ${GGML_CPU_ARM_ARCH:+-DGGML_CPU_ARM_ARCH=$GGML_CPU_ARM_ARCH} > stable-diffusion.cpp/build-null/cmake.log 2>&1 || { tail -20 stable-diffusion.cpp/build-null/cmake.log; exit 1; }
  cmake --build stable-diffusion.cpp/build-null -j"$JOBS" --target sd-cli > stable-diffusion.cpp/build-null/build.log 2>&1 || { tail -30 stable-diffusion.cpp/build-null/build.log; exit 1; }
  echo "stable-diffusion.cpp/build-null built"
}
shim() { ( cd cuda-shim && rm -rf "build/shim$SUF/nv" && make -s BUILD="build/shim$SUF" ${CUBIN_ARCH:+CUBIN_ARCH=$CUBIN_ARCH} ) && echo "shim built${SUF:+ for $ARCH} (build/shim$SUF): libtinynv build id $(strings "cuda-shim/build/shim$SUF/nv/libtinynv.a" | grep -oE '^[0-9a-f]{7}(-dirty)?$|^nogit(-dirty)?$' | head -1)"; }
# The archive AND the registry object that has to be linked beside it. One without the other links nothing, so they
# are built by one step rather than left to be remembered separately.
# build-ggml-cuda.sh points libggml-cuda.a at whatever it built last; a second card's build puts it back, so the default
# tree still links its own archive afterwards.
cuda() {
  prev=$(readlink cuda-shim/build/libggml-cuda.a 2>/dev/null || true)
  sh cuda-shim/build/build-ggml-cuda.sh && sh cuda-shim/build/build-backend-reg.sh
  if [ -n "$SUF" ] && [ -n "$prev" ]; then ln -sfn "$prev" cuda-shim/build/libggml-cuda.a; fi
}
# One link, with its failure kept: the output is filtered for display, but the status decides. $4, when given, is the
# CMake tree whose link line is replayed (stable-diffusion.cpp's rather than llama.cpp's).
link_one() {
  n=$3
  if out=$(if [ -n "${4:-}" ]; then L=$4 sh build/link-null.sh "$1" "$2" "$3"; else sh build/link-null.sh "$1" "$2" "$3"; fi 2>&1); then
    printf '%s\n' "$out" | grep -vE 'duplicate|warning' | tail -1
  else
    printf '%s\n' "$out" | grep -vE 'duplicate|warning' | tail -5
    echo "LINKING FAILED for $(basename "$n") - the reason is the last lines above, and nothing below this would have run"
    exit 1
  fi
}
# The archive and shim libraries for ARCH, named explicitly. libggml-cuda.a is a symlink to whichever card was built
# LAST, so after an  ARCH=sm_86 sh setup.sh cuda  on a tree that never built the default, a plain link silently put
# Ampere kernels into a Blackwell binary (2026-10-16). A tree from before the per-architecture archives has a real
# libggml-cuda.a file for sm_120a; that is still accepted.
card_libs() {
  GGML_ARCHIVE="$R/cuda-shim/build/libggml-cuda.$ARCH.a"
  if [ ! -f "$GGML_ARCHIVE" ] && [ -z "$SUF" ] && [ -f cuda-shim/build/libggml-cuda.a ] && [ ! -L cuda-shim/build/libggml-cuda.a ]; then
    GGML_ARCHIVE="$R/cuda-shim/build/libggml-cuda.a"
  fi
  SHIM_DIR="$R/cuda-shim/build/shim$SUF"; export GGML_ARCHIVE SHIM_DIR
  test -f "$GGML_ARCHIVE" || { echo "no libggml-cuda.$ARCH.a: build it with  ${SUF:+ARCH=$ARCH }sh setup.sh cuda  (nvcc in a container here, or on TINYCC_HOST)"; exit 1; }
  test -f "$SHIM_DIR/nv/libtinynv.a" || { echo "no $SHIM_DIR: build it with  ${SUF:+ARCH=$ARCH }sh setup.sh shim"; exit 1; }
}
link() {
  card_libs
  # Built here too, not only in the cuda step: it is cheap, it is the object every link below needs, and a tree that
  # has the archive but not this one used to fail seven times over and still report success.
  test -f cuda-shim/build/ggml-backend-reg.cuda.o || sh cuda-shim/build/build-backend-reg.sh
  cd cuda-shim
  for t in 'tests test-backend-ops' 'examples/simple llama-simple' 'examples/speculative-simple llama-speculative-simple' 'tools/server llama-server' 'tools/mtmd llama-mtmd-cli' 'tools/llama-bench llama-bench'; do
    d=${t%% *}; n=${t##* }; link_one "$d" "$n" "$R/cuda-shim/build/bin$SUF/$n-null"
  done
  if [ -f "$R/stable-diffusion.cpp/build-null/examples/cli/CMakeFiles/sd-cli.dir/link.txt" ]; then
    link_one examples/cli sd-cli "$R/cuda-shim/build/bin$SUF/sd-cli-null" "$R/stable-diffusion.cpp/build-null"
  fi
  ls -1 "build/bin$SUF"
}
# Metal + CUDA in one process (cuda-shim/build/link-null-metal.sh): the same llama.cpp checkout configured a second time
# with Metal on, then the server and llama-bench linked with the shim. Not part of "all": the CPU-only binaries are the
# shim's own tests, and this is a use of them. ARCH works as for link (bin-<arch>/*-mc); measured on Blackwell.
split() {
  card_libs
  mkdir -p llama.cpp/build-metal-null
  cmake -S llama.cpp -B llama.cpp/build-metal-null -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=ON -DGGML_NATIVE=${GGML_NATIVE:-ON} ${GGML_CPU_ARM_ARCH:+-DGGML_CPU_ARM_ARCH=$GGML_CPU_ARM_ARCH} -DLLAMA_CURL=OFF \
    -DLLAMA_OPENSSL=${LLAMA_OPENSSL:-ON} > llama.cpp/build-metal-null/cmake.log 2>&1 || { tail -20 llama.cpp/build-metal-null/cmake.log; exit 1; }
  cmake --build llama.cpp/build-metal-null -j"$JOBS" --target llama-server llama-bench \
    > llama.cpp/build-metal-null/build.log 2>&1 || { tail -30 llama.cpp/build-metal-null/build.log; exit 1; }
  L=$R/llama.cpp/build-metal-null sh cuda-shim/build/build-backend-reg.sh cuda-metal
  cd cuda-shim
  for t in 'tools/server llama-server' 'tools/llama-bench llama-bench'; do
    d=${t%% *}; n=${t##* }
    rm -f "$R/cuda-shim/build/bin$SUF/$n-mc"
    sh build/link-null-metal.sh "$d" "$n" "$R/cuda-shim/build/bin$SUF/$n-mc" 2>&1 | grep -vE 'duplicate|warning' | tail -1
    test -f "$R/cuda-shim/build/bin$SUF/$n-mc" || { echo "LINKING FAILED for $n-mc"; exit 1; }
  done
}
case "$step" in
  all)   deps; llama; sd; shim; cuda; link ;;
  deps)  deps ;; llama) llama ;; sd) sd ;; shim) shim ;; cuda) cuda ;; link) link ;; split) split ;;
  arch)  echo "$ARCH" ;;   # the ARCH this run resolved to (install.sh fixes it once for all its stages)
  *) echo "usage: sh setup.sh [all|deps|llama|sd|shim|cuda|link|split|arch]"; exit 2 ;;
esac

#!/usr/bin/env bash
# Fast ARM64/ARM64EC Windows PE DLL rebuild for Proton-Arm64.
#
# Rebuilds only the PE Windows DLLs (DXVK, VKD3D-Proton, FEX WoW64) and drops
# them straight into the existing Proton-Arm64 build's redist/ tree. Uses a
# native amd64 Ubuntu + llvm-mingw cross-compiler — NO QEMU emulation —
# typical cycle is 5-10 min vs 2h for the full Proton-Arm64 build.
#
# Assumes you've already done the full Proton-Arm64 build at least once:
#   /home/alberto/Documentos/Proton-Arm64/build/redist/files/lib/wine/aarch64-windows/
#   exists with the slow-built DLLs we're going to override.
#
# Stages:
#   image  — build proton-arm64-cross:local docker image (one-time, ~10 min)
#   fex    — libwow64fex.dll + libarm64ecfex.dll
#   dxvk   — d3d8/d3d9/d3d10core/d3d11/dxgi  (ARM64EC)
#   vkd3d  — d3d12 + d3d12core              (ARM64EC)
#   install — drop all DLLs into Proton-Arm64 redist/
#
# Usage:
#   ./build_arm64_pe_dlls.sh          # all stages
#   ./build_arm64_pe_dlls.sh dxvk install   # DXVK only, then install

set -euo pipefail

PROTON_SRC="/home/alberto/Documentos/Proton-Arm64"
PROTON_REDIST="${PROTON_SRC}/build/redist"
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGED="${HERE}/.arm64_pe_staged"
IMAGE_TAG="proton-arm64-cross:local"
JOBS="$(nproc)"

LLVM_MINGW_VERSION="20250910"
LLVM_MINGW_TARBALL="llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/${LLVM_MINGW_TARBALL}"

STAGES=("image" "fex" "dxvk" "vkd3d" "install")
RUN_STAGES=("$@")
[ ${#RUN_STAGES[@]} -eq 0 ] && RUN_STAGES=("${STAGES[@]}")

should_run() {
    for s in "${RUN_STAGES[@]}"; do
        [ "$s" = "$1" ] && return 0
    done
    return 1
}

run_in_image() {
    docker run --rm --platform linux/amd64 \
        -v "${PROTON_SRC}:/src:ro" \
        -v "${STAGED}:/out" \
        -w /work \
        "${IMAGE_TAG}" \
        bash -c "$1"
}

stage_image() {
    if docker image inspect "${IMAGE_TAG}" >/dev/null 2>&1; then
        echo "==> image present — skip"
        return
    fi
    echo "==> building ${IMAGE_TAG}"
    local tmp; tmp="$(mktemp -d)"
    cat > "$tmp/Dockerfile" <<DOCKERFILE
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y -q \\
    build-essential git cmake ninja-build pkg-config \\
    python3 python3-pip python3-venv python3-mako \\
    xz-utils wget ca-certificates \\
    mingw-w64-tools nasm \\
    glslang-tools spirv-tools \\
 && python3 -m venv /opt/meson-venv \\
 && /opt/meson-venv/bin/pip install --quiet --no-input 'meson>=1.3' \\
 && wget -q "${LLVM_MINGW_URL}" \\
 && tar xf ${LLVM_MINGW_TARBALL} -C /opt \\
 && mv /opt/llvm-mingw-* /opt/llvm-mingw \\
 && rm ${LLVM_MINGW_TARBALL} \\
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/opt/meson-venv/bin:/opt/llvm-mingw/bin:\$PATH
DOCKERFILE
    docker build --platform linux/amd64 -t "${IMAGE_TAG}" "$tmp"
    rm -rf "$tmp"
}

stage_fex() {
    echo "==> FEX DLLs (libwow64fex, libarm64ecfex)"
    local desc rev
    desc="$(git -C "${PROTON_SRC}/FEX" describe --abbrev=7 2>/dev/null || echo unknown)"
    rev="$(git -C "${PROTON_SRC}/FEX" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"

    run_in_image "$(cat <<SH
set -euo pipefail
cp -a /src/FEX /work/FEX && cd /work/FEX
echo '$desc' > .git_describe
echo '$rev'  > .git_rev

build_one() {
    local triple="\$1" label="\$2"
    local outdir=/work/fex-\$label
    rm -rf \$outdir && mkdir -p \$outdir && cd \$outdir
    cmake /work/FEX \\
        -DENABLE_FEXCORE_PROFILER=True -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=False \\
        -DCMAKE_TOOLCHAIN_FILE=/work/FEX/Data/CMake/toolchain_mingw.cmake \\
        -DMINGW_TRIPLE=\$triple \\
        -DOVERRIDE_VERSION='$desc' -DOVERRIDE_HASH='$rev' \\
        -DCMAKE_INSTALL_PREFIX=/work/fex-\$label-install -G Ninja
    ninja -j${JOBS} && ninja install
}

build_one aarch64-w64-mingw32 aarch64
build_one arm64ec-w64-mingw32 arm64ec

mkdir -p /out/fex
find /work/fex-aarch64-install /work/fex-arm64ec-install \\
    \\( -name 'libwow64fex.dll' -o -name 'libarm64ecfex.dll' \\) \\
    -exec install -m 644 {} /out/fex/ \\;
echo '=== FEX DLLs ==='
ls -la /out/fex/
SH
)"
}

_meson_cross_arm64ec() {
    cat <<'CROSS'
[binaries]
c         = 'arm64ec-w64-mingw32-clang'
cpp       = 'arm64ec-w64-mingw32-clang++'
ar        = 'arm64ec-w64-mingw32-ar'
strip     = 'arm64ec-w64-mingw32-strip'
windres   = 'arm64ec-w64-mingw32-windres'
widl      = 'arm64ec-w64-mingw32-widl'
pkgconfig = 'pkg-config'

[properties]
needs_exe_wrapper = true

[host_machine]
system     = 'windows'
cpu_family = 'aarch64'
cpu        = 'aarch64'
endian     = 'little'
CROSS
}

stage_dxvk() {
    echo "==> DXVK (ARM64EC)"
    local cross; cross="$(_meson_cross_arm64ec)"
    run_in_image "$(cat <<SH
set -euo pipefail
cp -a /src/dxvk /work/dxvk && cd /work/dxvk
cat > /work/cross.txt <<'EOF'
${cross}
EOF
meson setup /work/dxvk-build \\
    --cross-file /work/cross.txt --buildtype release \\
    --prefix /work/dxvk-install --bindir ''
cd /work/dxvk-build
ninja -j${JOBS}
ninja install
mkdir -p /out/dxvk
find /work/dxvk-install -name '*.dll' -exec install -m 644 {} /out/dxvk/ \\;
echo '=== DXVK DLLs ==='
ls -la /out/dxvk/
SH
)"
}

stage_vkd3d() {
    echo "==> VKD3D-Proton (ARM64EC)"
    local cross; cross="$(_meson_cross_arm64ec)"
    run_in_image "$(cat <<SH
set -euo pipefail
cp -a /src/vkd3d-proton /work/vkd3d-proton && cd /work/vkd3d-proton
cat > /work/cross.txt <<'EOF'
${cross}
EOF
meson setup /work/vkd3d-build \\
    --cross-file /work/cross.txt --buildtype release \\
    --prefix /work/vkd3d-install --bindir ''
cd /work/vkd3d-build
ninja -j${JOBS}
ninja install
mkdir -p /out/vkd3d
find /work/vkd3d-install -name '*.dll' -exec install -m 644 {} /out/vkd3d/ \\;
echo '=== VKD3D DLLs ==='
ls -la /out/vkd3d/
SH
)"
}

stage_install() {
    local target="${PROTON_REDIST}/files/lib/wine/aarch64-windows"
    if [ ! -d "$target" ]; then
        echo "ERROR: target missing — $target" >&2
        echo "  Run the full Proton-Arm64 build once first." >&2
        exit 1
    fi
    echo "==> installing fast-built DLLs into $target"
    local copied=0
    for dir in fex dxvk vkd3d; do
        if [ -d "${STAGED}/$dir" ]; then
            for f in "${STAGED}/$dir"/*.dll; do
                [ -f "$f" ] || continue
                sudo install -m 644 "$f" "$target/" 2>/dev/null \
                    || install -m 644 "$f" "$target/" \
                    || { echo "FAILED to install $f — target may need sudo"; exit 1; }
                copied=$((copied+1))
            done
        fi
    done
    echo "==> installed $copied DLLs"
    ls -la "$target" | grep -iE "libwow64fex|libarm64ecfex|d3d11|d3d12|dxgi" | head
}

mkdir -p "${STAGED}"
for s in "${STAGES[@]}"; do
    if should_run "$s"; then
        echo ""
        echo "====== STAGE: $s ======"
        case $s in
            image)    stage_image ;;
            fex)      stage_fex ;;
            dxvk)     stage_dxvk ;;
            vkd3d)    stage_vkd3d ;;
            install)  stage_install ;;
        esac
    fi
done

echo ""
echo "=== DONE ==="

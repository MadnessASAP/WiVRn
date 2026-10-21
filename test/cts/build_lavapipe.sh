#!/bin/bash
# Build Mesa's lavapipe (software Vulkan driver) into a prefix, for running the OpenXR CTS
# without a GPU when the distribution's Mesa is too old (VK_KHR_external_semaphore_fd and
# VK_KHR_external_fence_fd appeared in lavapipe with Mesa 26.2).
#
# Usage: build_lavapipe.sh PREFIX [VERSION]
# The driver manifest is PREFIX/share/vulkan/icd.d/lvp_icd.x86_64.json (pass it to
# run_cts.py --vk-driver).
#
# Build dependencies (Debian/Ubuntu): meson ninja-build pkg-config python3-mako python3-yaml
# python3-packaging bison flex glslang-tools llvm-dev libclang-dev libdrm-dev libelf-dev
# libexpat1-dev zlib1g-dev libzstd-dev libudev-dev libx11-xcb-dev libxcb-dri3-dev libxcb-present-dev
# libxshmfence-dev libxrandr-dev libwayland-dev wayland-protocols curl
set -euo pipefail

prefix=$(realpath -m "$1")
version=${2:-26.2.5}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL "https://archive.mesa3d.org/mesa-$version.tar.xz" | tar -xJ -C "$work"
cd "$work/mesa-$version"
meson setup build \
	--prefix="$prefix" \
	--buildtype=release \
	-Dgallium-drivers=llvmpipe \
	-Dvulkan-drivers=swrast \
	-Dplatforms= \
	-Dglx=disabled \
	-Degl=disabled \
	-Dgbm=disabled \
	-Dopengl=false \
	-Dgles1=disabled \
	-Dgles2=disabled \
	-Dllvm=enabled \
	-Dvalgrind=disabled \
	-Dlibunwind=disabled \
	-Dbuild-tests=false
ninja -C build -j"${JOBS:-$(nproc)}"
ninja -C build install
ls "$prefix"/share/vulkan/icd.d/

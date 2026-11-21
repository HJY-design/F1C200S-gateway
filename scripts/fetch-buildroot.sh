#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8
base=$(cd "$(dirname "$0")/.." && pwd)
install -d "$base"/{src,dl,logs,output,reference,artifacts,scripts}
cd "$base/dl"
if [[ ! -s buildroot-2025.02.18.tar.xz ]]; then
  wget --timeout=30 --tries=3 --no-verbose \
    https://buildroot.org/downloads/buildroot-2025.02.18.tar.xz
fi
xz -t buildroot-2025.02.18.tar.xz
sha256sum buildroot-2025.02.18.tar.xz | tee "$base/logs/buildroot-source.sha256"
if [[ ! -d "$base/src/buildroot-2025.02.18" ]]; then
  tar -xf buildroot-2025.02.18.tar.xz -C "$base/src"
fi
cd "$base/src/buildroot-2025.02.18"
printf 'BUILDROOT_VERSION\n'
head -n 8 Makefile
printf 'BOOTLIN_ARMV5\n'
grep -n -A 32 -B 3 'ARMV5_EABI_MUSL_STABLE' toolchain/toolchain-external/toolchain-external-bootlin/Config.in.options
printf 'DEFAULT_KERNEL\n'
grep -n -A 8 'config BR2_LINUX_KERNEL_LATEST_VERSION' linux/Config.in
printf 'AVAILABLE_BOARD_CONFIGS\n'
find configs -maxdepth 1 -name '*lichee*'

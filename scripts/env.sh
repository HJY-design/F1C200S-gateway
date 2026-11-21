#!/usr/bin/env bash
# Source this file: source ~/f1c200s-gateway/scripts/env.sh
export F1C_GATEWAY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export F1C_BUILDROOT="$F1C_GATEWAY_DIR/src/buildroot-2025.02.18"
export F1C_OUTPUT="$F1C_GATEWAY_DIR/output"
export F1C_EXTERNAL="$F1C_GATEWAY_DIR/buildroot-external"
export F1C_KERNEL_BUILD="$F1C_OUTPUT/build/linux-6.12.109"
export F1C_TARGET_CROSS="$F1C_OUTPUT/host/bin/arm-linux-"
export PATH="$F1C_OUTPUT/host/bin:$PATH"
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

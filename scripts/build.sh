#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8
export LANG=C.UTF-8
base=$(cd "$(dirname "$0")/.." && pwd)
[[ $(id -u) -ne 0 ]] || { echo 'Build as an unprivileged user, not root'; exit 1; }
free_kb=$(df -Pk "$base" | awk 'NR==2 { print $4 }')
[[ $free_kb -gt 1048576 ]] || { echo 'Less than 1 GiB free; stop before exhausting disk'; exit 2; }
exec make -C "$base/src/buildroot-2025.02.18" \
  O="$base/output" BR2_EXTERNAL="$base/buildroot-external" "${@:-all}"

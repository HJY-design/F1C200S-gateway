#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8
export DEBIAN_FRONTEND=noninteractive
[[ $(id -u) -eq 0 ]] || exit 1
apt-get update
apt-get install -y --no-install-recommends \
  build-essential git wget curl ca-certificates rsync unzip cpio bc gawk file \
  patch bison flex pkg-config libssl-dev libncurses-dev libelf-dev \
  device-tree-compiler python3 python3-dev python3-venv python3-pip \
  python3-setuptools python3-yaml swig autoconf automake libtool texinfo \
  help2man libusb-1.0-0-dev cmake ninja-build zstd xz-utils qemu-user-static \
  gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi gdb-multiarch sunxi-tools \
  libgpiod-dev
df -hT /

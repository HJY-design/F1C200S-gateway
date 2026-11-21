#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8
base=$(cd "$(dirname "$0")/.." && pwd)
source "$base/scripts/env.sh"
target="$F1C_OUTPUT/target"
install -d "$base/artifacts"
"${F1C_TARGET_CROSS}gcc" -mcpu=arm926ej-s -mfloat-abi=soft -Os \
  "$base/scripts/toolchain-smoke.c" -o "$base/artifacts/toolchain-smoke"
"${F1C_TARGET_CROSS}readelf" -h -A "$base/artifacts/toolchain-smoke"
qemu-arm-static -cpu arm926 -L "$target" "$base/artifacts/toolchain-smoke"
applets=$(qemu-arm-static -cpu arm926 -L "$target" "$target/bin/busybox" --list)
for applet in ash stty dd mdev udhcpc modprobe depmod start-stop-daemon; do
  grep -Fx "$applet" <<< "$applets"
done
qemu-arm-static -cpu arm926 -L "$target" "$target/usr/bin/openocd" --version
qemu-arm-static -cpu arm926 -L "$target" "$target/usr/bin/gpioinfo" --version
qemu-arm-static -cpu arm926 -L "$target" "$target/usr/sbin/dropbear" -V
qemu-arm-static -cpu arm926 -L "$target" "$target/usr/sbin/wpa_supplicant" -v
qemu-arm-static -cpu arm926 -L "$target" "$target/usr/sbin/iw" --version
kernel_cfg="$F1C_KERNEL_BUILD/.config"
for required in CONFIG_ARCH_MULTI_V5=y CONFIG_MACH_SUNIV=y CONFIG_CPU_ARM926T=y CONFIG_MODULES=y CONFIG_SPI_SUN6I=y CONFIG_SERIAL_8250_DW=y CONFIG_SERIAL_8250_NR_UARTS=3 CONFIG_SERIAL_8250_RUNTIME_UARTS=3 CONFIG_GPIO_CDEV=y CONFIG_GPIO_CDEV_V1=y CONFIG_USB_MUSB_SUNXI=y CONFIG_CFG80211=m CONFIG_BT=m CONFIG_MTD_SPI_NAND=y CONFIG_MTD_OF_PARTS=y CONFIG_MTD_UBI=y CONFIG_UBIFS_FS=y; do
  grep -Fx "$required" "$kernel_cfg"
done
module=$(find "$target/lib/modules" -type f -name esp32_spi.ko -print -quit)
[[ -n "$module" ]]
file "$module"
modinfo "$module"
dtb="$F1C_OUTPUT/images/suniv-f1c200s-gateway.dtb"
[[ $(fdtget -t x "$dtb" /memory@80000000 reg) == '80000000 4000000' ]]
[[ $(fdtget -t s "$dtb" /soc/usb@1c13000 dr_mode) == host ]]
[[ $(fdtget -t s "$dtb" /soc/mmc@1c0f000 status) == disabled ]]
[[ $(fdtget -t s "$dtb" /soc/spi@1c06000/wifi@0 compatible) == espressif,esp32-spi ]]
[[ $(fdtget -t s "$dtb" /soc/spi@1c05000/flash@0 compatible) == spi-nand ]]
[[ $(fdtget -t u "$dtb" /soc/spi@1c05000 num-cs) == 1 ]]
[[ $(fdtget -t s "$dtb" /aliases serial2) == /soc/serial@1c25800 ]]
[[ $(fdtget -t s "$dtb" /soc/serial@1c25800 status) == okay ]]
[[ $(fdtget -t s "$dtb" /soc/pinctrl@1c20800/uart2-pd-pins pins) == 'PD13 PD14' ]]
[[ $(fdtget -t s "$dtb" /soc/pinctrl@1c20800/uart2-pd-pins function) == uart2 ]]
if grep -E '^ttyS[12]:.*getty' "$target/etc/inittab"; then
  echo 'F407 / K230 UART must not run a login getty'; exit 1
fi
python3 - <<'PY'
import os, pathlib, subprocess
dtb = pathlib.Path(os.environ['F1C_OUTPUT']) / 'images/suniv-f1c200s-gateway.dtb'
raw = subprocess.check_output(['fdtget', '-t', 'bx', str(dtb), '/soc/pinctrl@1c20800', 'gpio-line-names'], text=True)
data = bytes(int(x, 16) for x in raw.split())
names = data.split(b'\0')[:-1]  # Preserve trailing unnamed/NC GPIO entries.
for offset, name in [(101, b'F407_RESET_ASSERT'), (102, b'F407_BOOT0_SET'), (132, b'')]:
    assert names[offset] == name, (offset, names[offset])
reset = subprocess.check_output(['fdtget', '-t', 'u', str(dtb),
                                '/soc/spi@1c06000/wifi@0', 'reset-gpios'], text=True)
# GPIO_ACTIVE_LOW=1; GPIO_OPEN_DRAIN=SINGLE_ENDED|OPEN_DRAIN=2|4.
cells = [int(x) for x in reset.split()]
assert cells[1:] == [3, 17, 7], cells
print('DTB GPIO: PD5 / PD6 names, PE4 NC, PD17 ACTIVE_LOW|OPEN_DRAIN verified')
PY
uboot="$F1C_OUTPUT/build/uboot-2025.01"
for required in CONFIG_SPL_SPI_SUNXI_GATEWAY_NAND=y CONFIG_SPL_LEGACY_IMAGE_CRC_CHECK=y CONFIG_DM_MTD=y CONFIG_MTD_SPI_NAND=y CONFIG_CMD_MTD=y CONFIG_ENV_IS_NOWHERE=y; do
  grep -Fx "$required" "$uboot/.config"
done
cmp "$uboot/arch/arm/mach-sunxi/gateway_spinand_spl.h" "$base/buildroot-external/board/gateway/gateway_spinand_spl.h"
gcc -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$uboot/arch/arm/mach-sunxi" "$base/scripts/test-spinand-spl.c" \
  -o "$base/artifacts/test-spinand-spl"
"$base/artifacts/test-spinand-spl"
python3 "$base/scripts/make-nand-images.py"
[[ -s "$target/lib/firmware/regulatory.db" ]]
[[ -s "$target/lib/firmware/regulatory.db.p7s" ]]
file "$F1C_OUTPUT/images/zImage" "$F1C_OUTPUT/images/rootfs.cpio.gz" "$F1C_OUTPUT/images/u-boot-sunxi-with-spl.bin"
du -sh "$target" "$F1C_OUTPUT/images"
df -h /
printf 'VERIFY_BUILD_PASS (cross compilation / QEMU userspace / DTB only; no hardware tests)\n'

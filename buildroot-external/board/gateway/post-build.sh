#!/bin/sh
set -eu
target=$1
chmod 0700 "$target/root/.ssh"
chmod 0600 "$target/root/.ssh/authorized_keys"
chmod 0755 "$target/usr/bin/f407-uart-log"
chmod 0755 "$target/usr/bin/gateway-remote"
chmod 0755 "$target/usr/bin/gateway-time" "$target/usr/bin/gateway-log-service"
chmod 0755 "$target/usr/bin/gateway-wifi-event" "$target/usr/bin/gateway-wifi-prefer"
chmod 0755 "$target/usr/bin/gateway-wifi-enable-configured" "$target/usr/bin/gateway-process-alive"
chmod 0755 "$target/etc/init.d/S12early-uart-log"
chmod 0755 "$target/etc/init.d/S44gateway-state" "$target/www/cgi-bin/index.cgi"
chmod 0755 "$target/etc/init.d/S42gateway-logger" "$target/etc/init.d/S43gateway-time" "$target/etc/init.d/S50gateway-httpd"
chmod 0755 "$target/etc/init.d/S15watchdog" "$target/etc/init.d/S41wifi"
chmod 0755 "$target/etc/init.d/S03uart-idle" "$target/usr/bin/gateway-uart-idle"
chmod 0755 "$target/etc/init.d/S46gateway-health" "$target/usr/bin/gateway-health" "$target/usr/bin/gateway-event"
chmod 0755 "$target/usr/share/udhcpc/default.script.d/S10gateway-dns" "$target/usr/share/udhcpc/default.script.d/S20gateway-mdns"
chmod 0600 "$target/etc/wpa_supplicant.conf"
mkdir -p "$target/data" "$target/etc/gateway"
grep -F -x -q '::respawn:/usr/bin/gateway-health' "$target/etc/inittab" || \
    printf '\n::respawn:/usr/bin/gateway-health\n' >> "$target/etc/inittab"
sed -i 's|^::ctrlaltdel:/sbin/reboot$|#::ctrlaltdel:/sbin/reboot|' "$target/etc/inittab"
ln -sf /run/gateway-status.json "$target/www/status.json"
cat > "$target/etc/gateway/build-info" <<'EOF'
Buildroot=2025.02.18
Linux=6.12.109
ESP-Hosted=f95889a9b62c6228654885291834e01a22610849
ESP-Reset-Patch=assert 10-12ms for this board EN RC
CPU=ARM926EJ-S ARMv5TE soft-float
RAM=64MiB; U-Boot mainline DDR clock 96MHz
SPI-WiFi=2MHz mode2; DT ceiling 2000000Hz
Stability-Update=2026-10-27 SPI pending-level fallback 20ms; init service supervision; saved Wi-Fi enable states
Hardware-Baseline=2026-10-13 NAND cold boot and Wi-Fi/DNS/HTTP passed
Hardware-Baseline-Report=09_F1C200S实机调试/验证记录
Gateway-Control=1.1.3; shared HTTP/SSH CLI; independent UART logger
Hardware-Update=2026-10-14 SPI2MHz reboot and bidirectional checksum passed; PD7 USB power switch passed
EOF

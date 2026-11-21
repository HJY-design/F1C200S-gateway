Development image. Hardware has not been tested.
Console: UART0 PE0/PE1, ttyS0, 115200n8.
F407 log UART: UART1 PD3/PD4, ttyS1; no getty on ttyS1.
gateway-ctl daemon holds PD5 released (active-low open-drain) and PD6 low.
It finds GPIOs by their kernel line names, not guessed chip numbers/offsets.
Use gateway-ctl normal before UART logging. Stop the logger before ROM download.
gateway-ctl reset pulses NRST; rom requests BOOT0 then pulses NRST.
Direct-reset board (2026-10-05): PD5 drives NRST low only, then releases to input.
PD6 sets BOOT0; a target-side pull-down must select normal boot before Linux.
PE4 is NC. gateway-ctl disable returns an error: there is no UART/BOOT isolation gate.
Wi-Fi: modprobe esp32_spi, then check dmesg/iw dev before association.
Copy the example Wi-Fi config to /etc/wpa_supplicant.conf and fill it in.
OpenOCD is started manually and binds to 127.0.0.1; use SSH port forwarding.
Root uses SSH public-key login. This image contains only the PC public key.
The serial development console accepts root with no password.
No PC private key or VM password is included.
No fixed NOR/NAND partition map, automatic NAND format, or flash command is set.
Keep board-specific startup and memory tests separate from compilation results.

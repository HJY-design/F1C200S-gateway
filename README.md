# F1C200S Linux 无线调试与 AI 协作网关

把串口记录、固件下载和设备调试放到目标板旁边。全志 F1C200S 从板载 SPI-NAND 启动 Linux，ESP32-C3 通过 SPI 接入 Wi-Fi。电脑可以用网页查看日志和管理固件，也可以经 SSH 运行网关命令。AI 编程助手能沿用同样的入口，在电脑上分析和构建，在网关上部署并读取实机反馈。

![F1C200S 网关 PCB 工程预览](docs/pcb-preview.png)

## 已实现的功能

- UART1、UART2 持续记录原始字节与接收时间索引，网页支持文本、HEX 和整份导出。
- USB Host 接 ST-Link，由 OpenOCD 写入、校验并复位目标板。已有 STM32F103 和 STM32F407 联调记录。
- 网页、CLI 和脚本可查看网关状态、对接目标协议、保存参数组。现有 F407 适配是一个应用实例，可以换成自己的协议。
- Wi-Fi 重连、局域网发现、日志保留和服务监督均在板上运行。Linux 环境可以继续加入自己的驱动、工具和网络服务。

AI 协作的一次实际用法是先读取串口和目标状态，再在电脑上构建一版固件，使用网关部署，最后对照下载校验、日志和现场观察。用于建立基线的只读命令有 `gateway status`、`gateway diagnose`、`gateway log uart1 --tail 4096`。接好调试线并明确目标以后，再执行烧录或参数写入。

## 从哪里看

| 内容 | 入口 |
| --- | --- |
| Altium Designer 工程、原理图和 PCB | [Board1_1.PrjPcb](hardware/altium/Board1_1.PrjPcb) · [P1.schdoc](hardware/altium/Board1_1/Schematic1_1/P1.schdoc) · [PCB1_1.pcbdoc](hardware/altium/Board1_1/PCB1_1.pcbdoc) |
| 嘉立创 EDA 专业版源工程 | [F1C200S-gateway.epro](hardware/easyeda/F1C200S-gateway.epro) |
| Buildroot 板级配置、设备树、引导补丁和系统文件 | [buildroot-external](buildroot-external/) |
| 网关 C 后台、CLI、日志工具和测试 | [gateway-control](buildroot-external/package/gateway-control/) |
| 网页前端 | [www](buildroot-external/board/gateway/rootfs-overlay/www/) |
| ESP-Hosted 驱动与 ESP32-C3 固件配置 | [Linux 驱动](buildroot-external/vendor/esp-hosted-linux/) · [ESP32-C3](firmware/esp32-c3/) |
| 构建、NAND 镜像生成与验证 | [scripts](scripts/) |
| 硬件、软件与扩展说明 | [设计与调试记录](docs/gateway-design-and-extension.md) · [立创开源广场](https://oshwhub.com/a-ordinary-people/project_okgonhpw) |

AD 文件从同一版嘉立创工程导出。已核对工程引用、81 个 PCB 元件计数及 55 个网络名称。铜皮、钻孔、规则及焊盘几何仍要以嘉立创源工程和生产输出复核，详见[硬件说明](hardware/README.md)。

## 构建和首次配置

网关使用 Buildroot 2025.02.18、Linux 6.12.109、U-Boot 2025.01，目标为 ARM926 / ARMv5 EABI 软浮点。使用具备 [Buildroot 依赖](https://buildroot.org/downloads/manual/manual.html#requirement)的 Linux 主机，在仓库根目录执行：

```sh
./scripts/fetch-buildroot.sh
./scripts/build.sh f1c200s_gateway_defconfig
./scripts/build.sh
```

工具链、内核和用户空间第三方软件由 Buildroot 拉取。网关应用、板级配置和构建脚本保存在本库。ESP32-C3 的固件配套信息见[固件目录](firmware/esp32-c3/README.md)。

上电先核对接线和供电，从 UART0 检查启动。从板载 SPI-NAND 启动所需的 SPL/U-Boot、FIT 和 UBI/UBIFS 分开准备，布局由板级设备树和 `scripts/make-nand-images.py` 定义。根文件系统压缩包及普通 U-Boot 二进制各有用途，不能直接按整片 NAND 镜像写入。

初次配置建议在自己的调试网络进行。Wi-Fi 配置参见 `buildroot-external/board/gateway/rootfs-overlay/etc/gateway/wpa_supplicant.conf.example`，在自己的设备上填写网络信息。SSH 使用自己的公钥。常用本地检查入口在 `buildroot-external/package/gateway-control/tests/run-native.sh`，ARM 构建产物可用 `scripts/verify-build.sh` 进一步检查。

## 许可

硬件工程沿用立创项目的 GPL 3.0，见 [hardware/LICENSE](hardware/LICENSE)。网关 C 应用保留原有的 [MIT 许可](buildroot-external/package/gateway-control/src/LICENSE)，ESP-Hosted 和 cJSON 各自保留随源码提供的许可。Buildroot 获取的第三方软件继续适用其各自许可。

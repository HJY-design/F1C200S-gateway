# ESP32-C3 无线协处理器

板级配置片段 `sdkconfig.fragment` 与 Linux 侧 ESP-Hosted 配合使用。基线固定为 [Espressif esp-hosted-linux f95889a](https://github.com/espressif/esp-hosted-linux/tree/f95889a9b62c6228654885291834e01a22610849)，上游固件工程位于 `esp/esp_driver/network_adapter`，目标是 `esp32c3`。上游此版本记录的 ESP-IDF 为 `fff9895c82d744c7237be8847347bdd1b07c6643`。

在匹配的 ESP-IDF 环境中，以板级片段配置 SPI 接口、握手、数据就绪和校验选项，并参照该版本上游固件说明构建。片段只记录项目相关选项。历史样板报告 `NG-1.0.6.0.13`，从固定上游源码重新构建的结果需要与这里的 Linux 驱动配对验证。

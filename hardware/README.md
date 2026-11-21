# F1C200S 网关硬件

从 2026-11-21 立创审核版导出，原理图主控标注为 F1C200S，板上无线协处理器为 ESP32-C3。

- `altium/Board1_1.PrjPcb` 引用同目录树下的原理图和 PCB。下载时保留目录结构。
- `easyeda/F1C200S-gateway.epro` 保存嘉立创 EDA 专业版原工程。用于核对板框、层、器件、网络和铺铜。

静态比较中，两种格式都有 81 个 PCB 元件和相同的 55 个 PCB 网络名称。AD 导出记录有 136 条 Via，嘉立创源文件有 127 条，暂未逐条确认转换关系。生产前以原生工程为基准核对焊盘、孔、叠层、铜皮和设计规则，并检查重新生成的生产文件。格式转换限制见[嘉立创官方说明](https://prodocs.lceda.cn/cn/import-export/export-altium-designer/)。

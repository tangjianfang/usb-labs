# usbsim · USB/BLE/PD 协议逻辑仿真器

> 在纯软件中模拟 USB 协议的**通信链路行为**：虚拟主机轮询虚拟设备，包真编解码（CRC5/CRC16 按规范算法）、真状态机（枚举/BOT/ATT/PD 协商）、可错误注入、全程产出标注抓包流。

## ⚠️ 仿真边界（诚实声明）

| 层 | 仿真的 | 不仿真的 |
|---|---|---|
| 电气层 | — | 波形、NRZI/位填充、BMC 载波、LFPS 时序 |
| 协议层 | ✅ 包编解码（CRC 实算）、事务、状态机、切换逻辑 | — |
| 链路层 | ✅ 帧节拍、NAK 重试、toggle、Split 概念 | 精确 µs 时序 |

需要真实电气行为的验证 → 仍需硬件分析仪（见 [../docs/02-抓包分析实战.md](../docs/02-抓包分析实战.md)）。

## 架构

```
simulator/usbsim/
├── packets.py   包编解码: PID/CRC5/CRC16/Token/Data/Handshake + 解析
├── bus.py       虚拟总线: 帧计数、事务路由、捕获日志、错误注入
├── device.py    设备模型: EP0 状态机、描述符服务、Hub/HID/MSC 功能
├── host.py      主机模型: 标准枚举序列、control/bulk/interrupt API
├── pd.py        PD 消息层: Source/Sink 策略引擎、PDO/RDO、协商时序
├── ble.py       BLE: 广播/连接/GATT 属性表/订阅通知
└── capture.py   抓包流导出: JSONL + 标注文本
```

## 快速开始

```bash
# 枚举一台虚拟 HID 复合设备（键盘+鼠标）并轮询报告
python simulator/examples/run_enumeration.py

# MSC BOT: 写入扇区并读回校验
python simulator/examples/run_msc_rw.py

# PD: Source/Sink 协商 9V 合同
python simulator/examples/run_pd_negotiation.py

# 全部自测
python simulator/tests/test_sim.py
```

## 测试如何闭环（与产测框架的关系）

`tools/usbtest/` 的 mock 后端可以被本仿真器替代升级：
`usbtest` 步骤 → 驱动 usbsim 的虚拟设备 → 虚拟总线交换包 → 断言真实协议行为。
当前 usbtest 的 mock 是确定性占位；接入 usbsim 后即可在 CI 中演练**完整协议交互**。

## 已仿真的协议行为（自测覆盖）

- USB 2.0: 枚举全序列（描述符/地址/配置）、EP0 三阶段、批量 BOT+SCSI（INQUIRY/容量/写读）、中断轮询+NAK、数据触发、错误注入重传
- PD: Source_Capabilities 广播、RDO 构造与 PDO 选择（毫伏统一口径）、Accept/PS_RDY
- BLE: 广播载荷（AD 结构）、连接、GATT 发现/读/订阅

## Roadmap

- HS 微帧调度（125µs 精细化）
- Wireshark pcap 导出（LINKTYPE_USB_LINUX_MMAPPED）
- 更多设备类（UVC 流、UAC 异步反馈）
- EP-2 批准后：GATT Supplement 标准 UUID 全表

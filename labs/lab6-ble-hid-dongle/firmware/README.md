# Lab6 固件 · 构建说明（Nordic NCS 主线）

> 原则（同全仓库诚实声明）：**以官方 SDK 示例为起点，本实验室不重写协议栈**。
> 本文给出构建路径与示例位置；关键文件的骨架与集成点见 [src/架构说明.md](src/架构说明.md)。

## 1. 环境与构建（nRF Connect SDK / NCS）

NCS 基于 Zephyr，自带 SoftDevice Controller（或可选 Zephyr LL），HOGP、USB HID、ESB、绑定存储全部有官方实现。

1. 安装：nRF Connect for Desktop → Toolchain Manager（图形安装）或命令行：

```bash
nrfutil toolchain-manager launch --shell   # 进入带工具链的 shell
# 若用纯 west 工作流（版本号以官方发布页为准，下例 v2.6.x）：
west init -m https://github.com/nrfconnect/sdk-nrf --mr v2.6.1
west update
```

2. 构建 HOGP 键盘（**起步示例，就是商业键盘的主干**）：

```bash
cd <ncs>/nrf/samples/bluetooth/peripheral_hids_keyboard
west build -b nrf52840dk/nrf52840    # 旧版 NCS 板名写法为 nrf52840dk_nrf52840
west flash
```

- 示例官方文档：https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/samples/bluetooth/peripheral_hids_keyboard/README.html
- 该示例已包含：HOGP 服务（键盘 Report Map）、SMP 配对、绑定存储（settings 子系统）、电池服务（BAS）。

3. dongle 侧 USB HID：Zephyr/NCS 里的 USB HID 示例（如 `zephyr/samples/subsys/usb/hid` 一带，随 NCS 版本目录名有差异，以你安装版本为准）。
4. dongle 侧 2.4G 接收：NCS 自带 **ESB（Enhanced ShockBurst）库**（`lib/esb`）与示例；私有协议也可选择"G dongle 即 BLE Central"路线（见架构说明 §2，两种路线利弊都在那里）。

## 2. 调试手段

- **RTT 日志**：SEGGER RTT（`west build -t rtt` 或 nRF Connect for Desktop → SEGGER RTT）。串口 UART 也可，但会占用引脚并增加功耗。
- **蓝牙抓包**：nRF Sniffer for Bluetooth LE + Wireshark，见 [../host/btmon.md](../host/btmon.md)。
- **功耗**：PPK2（Power Profiler Kit II，数百元人民币量级）测连接事件电流，验证 32k 晶振与 DC/DC 配置是否生效。

## 3. 工程结构约定（本实验室的增量）

在官方示例基础上，本实验室约定四个自研文件（骨架见 [src/架构说明.md](src/架构说明.md)）：

| 文件 | 职责 | 基础来源 |
|---|---|---|
| `report_desc.c` | **共享**的 HID 报告描述符（USB 与 HOGP Report Map 同源） | Lab1 描述符 |
| `ble_hid.c` | HOGP 服务注册、报告发送、白名单/配对按钮策略 | NCS `peripheral_hids_keyboard` |
| `link_2g4.c` | dongle 私有 2.4G 链路（ESB）或 dongle-BLE 链路 | NCS ESB 库 |
| `usb_hid.c` | dongle：2.4G 收包 → USB HID 中断端点上报 | Zephyr USB HID |

> 完整可编译工程以 NCS 示例 + 上述集成点为基础自行拼装——拼装过程本身就是本实验室的核心练习。API 名以你所装 NCS 版本的头文件为准（NCS 迭代快，跨版本 API 有变动）。

## 4. ESP-IDF 路线（一句带过）

乐鑫路线用 ESP-IDF 自带 BLE HID 设备示例（`examples/bluetooth/bluedroid/ble/ble_hid_device_demo` 一带，仓库内路径随版本变化），USB 侧注意 ESP32-C6 无通用 USB 设备控制器、需换 ESP32-S3；其余思路与 NCS 路线一致，本实验室不展开。

## 参考资源

- NCS 官方文档首页：https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/
- Zephyr Bluetooth 文档：https://docs.zephyrproject.org/latest/
- nRF52840 DK / Dongle：https://www.nordicsemi.com/Products/nRF52840-Dongle
- 知识库：[07-HOGP-HIDoverGATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/07-HOGP-HIDoverGATT.md) · [06-SMP安全与配对](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/06-SMP安全与配对.md) · [HID 概述与定位](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/00-HID概述与定位.md)

# Lab7 抓包解读 · Dock 枚举拓扑（基于规范重构）

> ⚠️ **诚实声明**：以下拓扑与枚举输出为**基于 USB 3.x/USB4/Type-C 规范重构的教学样本**，
> 句柄/序列号/时间戳为教学编造，**不是真实采集数据**。真实调试方向见 §3。
> 背景知识：[05-AlternateMode与E-marker](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/05-AlternateMode与E-marker.md)、
> [02-USB4与雷电整合](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/02-USB4与雷电整合.md)、
> [05-USB4深入-路由隧道与配置](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/05-USB4深入-路由隧道与配置.md)。

## 1. 关键认知：坞站 = USB hub + Billboard + DP 音视频复合体的"三重身份"

主机视角下，一台 DP Alt Mode 扩展坞同时扮演三种角色，走在**三条独立通路**上：

| 角色 | 通路 | 主机看到什么 |
|---|---|---|
| USB Hub | USB3 总线（或 USB4 内的 USB3 隧道） | hub 设备 + 其下挂的网卡/读卡器等一串标准 USB 设备 |
| Billboard | USB 总线上的一台小设备（独立于 hub 分支与否，属类设备 0x11） | "USB Billboard Device"，承载 Alt Mode 状态诊断 |
| 显示输出 | DP 链路（Alt Mode 直连或 USB4 DP 隧道）——**不走 USB 枚举** | 显示系统里的显示器，与 usbmon/lsusb 无关 |

调试坞站问题的第一课：**分清这条证据属于哪条通路**。显示器黑屏去抓 USB 包 = 南辕北辙。

## 2. 重构样本：`lsusb -t` 视角的坞站拓扑

（教学重构，对应一台典型 DP Alt Mode 坞站：上行 Type-C，下行 HDMI×2 + USB-A×3 + 2.5GbE + SD 读卡器 + PD 100W 直通）

```
/:  Bus 02.Port 1: Dev 1, Class=root_hub, xHCI
    └─ Port 2: 5000M, Dev 3  ← 坞站上行（USB3.2 Gen2 经 Alt Mode 同线缆）
        ├─ 5000M Dev 4: USB3 Hub（坞站 hub 控制器）
        │   ├─ 5000M Dev 5: 2.5GbE 网卡（RTL8156 类）
        │   ├─ 480M  Dev 6: SD 读卡器（UASP MSC）
        │   └─ 480M  Dev 7: [空口即未插设备]
        └─ 12M   Dev 8: Billboard Device（Class=Vendor-specific，bDeviceClass=0x11 按
                 USB-IF Billboard 分配）      ← 注意：通常独立成枝，不挂 hub 下
```

要点解读：

1. **hub 枝**：坞站的 USB3 hub 是"树干"，下行口全是普通设备；`lsusb -t` 的速率列直接暴露"网卡插在 Gen2 口还是掉到了 480M"这类配置错误。
2. **Billboard 枝**：一台看起来"什么也不干"的小设备。它只在 Alt Mode 异常时才有存在感（见下）。
3. **看不见的枝**：显示走 DP 链路，`lsusb` 里没有；这也是"lsusb 全正常但屏幕黑了"这类工单的结构性原因。
4. **USB4 款的差异**：上行变为 USB4 路由器（设备管理器/boltctl 里出现 USB4/雷电设备），USB3 hub、Billboard 挂在其 USB3 域，显示经 DP 隧道——树形不变，物理通路换代（见知识库 USB4 篇）。

Billboard 的存在意义（Windows 用户可复现）：DP 未能进入 Alt Mode（线缆不支持、带宽不够、协商失败）时，Windows 弹出的"USB-C 设备功能受限/显示连接问题"提示，数据来源就是 Billboard 的 **SuperSpeed Link / Alt Mode 状态描述符**。没有 Billboard 的坞站，故障只剩"黑屏"两个字。

## 3. 真实调试方向（工具与分工）

| 层 | 工具 | 能看什么 | 量级 |
|---|---|---|---|
| USB 枚举 | `lsusb -t` / USBTreeView(Win) / IORegistryExplorer(mac) | 拓扑、速率、描述符、Billboard 状态 | 免费 |
| USB3/2 流量 | usbmon(Linux) / xhci 事件、USBPcap(Win) + Wireshark、或中端协议分析仪 | 端点流量、错误、hub 事件 | 免费～数万元 |
| PD/CC | PD 协议分析仪（多款厂商有售，插在线缆中间解码 SOP/SOP'/VDM） | 功率协商、**Alt Mode Enter Mode VDM 全过程** | 数万元级 |
| USB4/雷电 | 专业分析仪（Ellisys USB4 Explorer、Teledyne LeCroy Voyager M4x 类） | USB4 隧道、CM 消息、路由配置——协议级"CT" | 数十万元级，通常委托实验室 |
| 系统日志 | Linux：`boltctl list`、`/sys/bus/thunderbolt/devices/`、`dmesg`（thunderbolt/usb4 驱动日志）；Windows：设备管理器 USB4 Host Router/集线器节点、USB4 相关事件与 CM 日志、厂商坞站助手 | CM 视角的接入/认证/隧道建立 | 免费 |

实践路线（零成本起步）：

1. 先用 `lsusb -t`/设备管理器把 §2 的三重身份在自己手上认一遍；
2. 拔显示器线 → 看 Billboard 提示与描述符（Linux 可 `lsusb -v` 找 Billboard 类设备）；
3. Linux 上 `boltctl` 跟踪 USB4 设备接入全过程，对照知识库 [09-USB4-CM指南要点](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/09-USB4-CM指南要点.md) 理解 CM 状态；
4. 有 PD 分析仪条件时，录制一次完整"插线 → PD 协商 → VDM Enter Mode → DP link training"序列——这就是本文件 §1 表格里三条通路的现场版。
5. 公开真实 USB 抓包样例可参考 Wireshark 样例库（https://wiki.wireshark.org/SampleCaptures ）作对照。

## 参考资源

- USB-IF 官方（USB4 规范、Type-C/PD 规格、Billboard Device Class 规格下载）：https://www.usb.org/
- DisplayPort Alt Mode（VESA 官方资料）：https://www.displayport.org/
- 雷电/USB4 开发者资源：https://thunderbolttechnology.net/
- Wireshark 样例抓包库（公开真实样例）：https://wiki.wireshark.org/SampleCaptures
- 知识库：[05-AlternateMode与E-marker](https://github.com/tangjianfang/USBTree/blob/main/30-枝干-接口与供电/05-AlternateMode与E-marker.md) · [07-USB4规范级-配置空间与隧道](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/07-USB4规范级-配置空间与隧道.md) · [09-USB4-CM指南要点](https://github.com/tangjianfang/USBTree/blob/main/40-枝干-高速演进/09-USB4-CM指南要点.md)

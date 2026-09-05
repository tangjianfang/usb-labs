# Wireshark USB 显示过滤器速查

> 用途：分析 USB 抓包（usbmon / USBPcap / 硬件分析仪导出的 pcapng）。
> 前缀约定：`usb.*` 为 USB 协议栈公共字段；`ums.*`（BOT）、`scsi.*`/`scsi_sbc.*`（SCSI）、
> `usbhid.*`（HID）等类协议字段在启用对应子解析器后出现。
> 提示：字段名以你本机 Wireshark 的自动补全为准（过滤器输入框按 Ctrl+空格）——
> 各版本字段略有增删，标 ⚠ 的条目请先自动补全确认再使用。

## 1. 锁定"谁在哪"

| 过滤器 | 含义 |
|---|---|
| `usb.src == "host"` | 主机发出的记录（usbmon 视角） |
| `usb.dst == "1.5.1"` | 发往 总线1/设备5/端点1 |
| `usb.device_address == 5` | 某设备的全部流量 |
| `usb.endpoint_address == 0x81` | EP1 IN（bit7=方向, bit3~0=端点号） |
| `usb.bus_id == 2` | 某条总线 |
| ⚠ `usbmon.bus_id == 2` | usbmon 伪头部层（先抓到 usbmon 记录才有效） |

## 2. 传输类型与方向

| 过滤器 | 含义 |
|---|---|
| `usb.transfer_type == 0x00` | 同步（isochronous） |
| `usb.transfer_type == 0x01` | 中断 |
| `usb.transfer_type == 0x02` | 控制 |
| `usb.transfer_type == 0x03` | 批量 |
| `usb.endpoint_address.direction == 1` | IN 方向（若字段存在; 否则用 0x80 掩码判断） |
| ⚠ `usb.urb_type == 'S'` / `'C'` | URB submit / complete（配对查挂死） |
| `usb.data_len > 0` | 带载荷的记录 |
| `usb.capdata` | 有原始数据片段的包（右键 → 复制 → 十六进制流可导出） |

## 3. 控制传输与标准请求（枚举分析主力）

| 过滤器 | 含义 |
|---|---|
| `usb.setup.bmRequestType == 0x80` | 标准 IN 请求（0x00 标准 OUT, 0x21/0xA1 类） |
| `usb.setup.bRequest == 0x06` | GET_DESCRIPTOR |
| `usb.setup.bRequest == 0x05` | SET_ADDRESS |
| `usb.setup.bRequest == 0x09` | SET_CONFIGURATION |
| `usb.setup.bRequest == 0x08` | GET_CONFIGURATION |
| `usb.setup.wValue == 0x0200` | 描述符类型 0x02（配置） |
| `usb.setup.wLength == 64` | 首次读设备描述符的"探长" |
| `usb.bDescriptorType == 0x02` | 配置描述符应答 |
| `usb.bDescriptorType == 0x01` | 设备描述符应答（⚠ 部分版本字段名为 `usb.descriptor_type`） |
| `usb.idVendor == 0x2341` | 按 VID 过滤 |
| `usb.idProduct == 0x0001` | 按 PID 过滤 |
| `usb.bDeviceClass == 0x00` | 设备级类代码 |
| `usb.bInterfaceClass == 0x03` | HID 接口（0x08=MSC, 0x01=Audio, 0x0A=CDC-Data, 0xE0=BT） |

## 4. HID（键鼠）

| 过滤器 | 含义 |
|---|---|
| `usb.bInterfaceClass == 0x03` | HID 接口流量（配合 `&& usb.src==…` 收窄） |
| `usb.transfer_type == 0x01 && usb.data_len == 6` | 6 字节鼠标报告 |
| ⚠ `usbhid.setup.bRequest == 0x09` | SET_REPORT 类请求（HID 类请求经 usbhid 前缀解析） |
| ⚠ `usbhid.bDescriptorType == 0x22` | 报告描述符相关 |

## 5. MSC / BOT（U 盘）

| 过滤器 | 含义 |
|---|---|
| `ums.dCBWSignature == 0x43425355` | 只看 CBW |
| `ums.dCSWSignature == 0x53425355` | 只看 CSW |
| `ums.bCSWStatus == 0x01` | 失败的命令（随后必有 REQUEST SENSE） |
| `ums.bCSWStatus == 0x02` | 阶段错误（要复位恢复） |
| `ums.dCBWTag == 0x0000000a` | 追踪某条命令全程（CBW→Data→CSW） |
| `ums.bCBWLUN == 1` | 多 LUN 读卡器的第二个卡槽 |
| `scsi.opcode == 0x28` | READ(10)（0x2A 写, 0x12 INQUIRY, 0x03 REQUEST SENSE, 0x25 READ CAPACITY） |
| ⚠ `scsi_sbc.rdwr10.lba` | 提取读写 LBA 列（配 `-T fields -e` 导出） |
| ⚠ `scsi.sense.key == 0x02` | Sense Key（0x05 非法请求, 0x06 Unit Attention） |

## 6. Audio / UAC

| 过滤器 | 含义 |
|---|---|
| `usb.transfer_type == 0x00` | 同步流量本体（音频数据与反馈都在这） |
| `usb.bInterfaceClass == 0x01` | Audio 接口/端点 |
| ⚠ `usb.audio.endpoint.clock.frequency` | 反馈值（UAC 同步端点解析器；字段名随版本变化） |

## 7. 故障定位组合拳

| 目标 | 过滤器组合 |
|---|---|
| 枚举全程 | `usb.transfer_type == 0x02 && usb.endpoint_address == 0` |
| 枚举失败点 | `usb.transfer_type == 0x02 && !(usb.urb_type == 'S')`（只看 complete, 找 status 异常） |
| URB 提交后无完成（挂死） | `usb.urb_type == 'S' && !(usb.urb_type == 'C')` 不可行——用 `tshark -T fields -e usb.urb_id -e usb.urb_type` 导出后对比配对 |
| STALL 结果 | ⚠ `usb.urb_status < 0`（-EPIPE=-32 对应 STALL） |
| 某端点 bInterval 实测 | `usb.src == "1.5.1"` + 时间戳直方图（Statistics → IO Graph） |

## 8. 常用命令行（tshark）

```bash
# 只导出控制传输的详细解析（文本）
tshark -r cap.pcapng -Y "usb.transfer_type==0x02" -V > ctrl.txt

# 导出某设备 EP1 IN 的全部载荷
tshark -r cap.pcapng -Y 'usb.src=="1.5.1"' -T fields -e frame.number -e usb.capdata

# 列出所有标准请求（bRequest 列）
tshark -r cap.pcapng -Y "usb.setup.bRequest" -T fields -e usb.setup.bmRequestType -e usb.setup.bRequest -e usb.setup.wValue

# 统计中断端点实际交付间隔（验证 bInterval）
tshark -r cap.pcapng -Y 'usb.src=="1.5.1"' -T fields -e frame.time_relative
```

## 9. 协议启用/颜色提示

- usbmon/USBPcap 数据自动挂 USB 解析器；硬件分析仪（OpenVizsla 等）导出的 pcap 需手动
  "Decode As" 或选用对应封装插件；
- 类协议（SCSI/HID/UAC）子树是否展开取决于描述符是否被成功解析——枚举段坏掉时
  后续类字段全部消失，先修枚举再看类流量；
- 着色规则：Preferences → Appearance → Coloring Rules 里 USB 相关条目可标红 URB error。

## 参考资源

- Wireshark USB 文档: https://www.wireshark.org/docs/dfref/u/usb.html （字段全集）
- usbmon 抓包环境搭建: 见同目录 [usbmon_howto.md](usbmon_howto.md)
- Wireshark 样例捕获（练手素材）: https://wiki.wireshark.org/SampleCaptures
- 知识库对照: USBTree `70-枝干-调试测试与安全/01-协议分析仪与抓包.md`
- 本项目教学样本（配合练习）: `../captures/`（注意: 为规范重构样本, 练手请用真实 trace）

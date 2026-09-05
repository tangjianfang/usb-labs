# UVC 流建立与载荷帧分析（含 UAC 反馈端点解读）

> **诚实声明**：本文的帧序列为**基于 UVC 1.5 / UAC 1.0 规范重构的示意序列**（数值按规范与常见实现推演），**不是真实抓包记录**。真实抓包的操作指引见 §6，抓到的 .pcapng 建议存入仓库根目录 `captures/`（命名建议 `lab4-uvc-*.pcapng` / `lab4-uac-*.pcapng`），并把结论回填到本文。
> 知识库对照：[UVC 详解 §4/§5](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md) · [UAC1.0 详解 §3 端点与反馈](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md) · 固件侧源码：[../firmware/src/uvc_descriptors.c](../firmware/src/uvc_descriptors.c) / [../firmware/src/mjpeg_frame_fill.c](../firmware/src/mjpeg_frame_fill.c)

---

## 1. 一图看懂要抓的四样东西

```
① SET_INTERFACE 换挡（标准请求，无数据阶段）
② PROBE / COMMIT 类请求（EP0 控制传输，34 字节数据阶段）
③ VS 同步 IN 载荷流（每个包 = 2~12B 载荷头 + JPEG 切片，FID 翻转分帧）
④ UAC：音频同步 IN 流（录音）+ 反馈 IN 端点（播放方向异步设备才有，10.14 数值）
```

---

## 2. 流建立控制序列（基于 UVC 1.5 规范重构）

### 2.1 停流 + Probe 协商

| # | 包类型 | bmReqType | bRequest | wValue | wIndex | wLength | 数据阶段 | 解读 |
|---|---|---|---|---|---|---|---|---|
| 1 | SETUP | 0x01 | 0x0B SET_INTERFACE | 0x0000 | 0x0001 | 0 | — | VS 接口切 **Alt0**：停流、归还带宽 |
| 2 | SETUP | 0xA1 | 0x86 GET_INFO | 0x0100 | 0x0001 | 1 | IN: 0x03 | VS_PROBE_CONTROL 能力位（可读可写） |
| 3 | SETUP | 0xA1 | 0x82 GET_MIN | 0x0100 | 0x0001 | 34 | IN: 34B | 最小 Probe（主机探边界，可选） |
| 4 | SETUP | 0x21 | 0x01 SET_CUR | 0x0100 | 0x0001 | 34 | OUT: 34B | 主机**提议**：格式1(MJPEG)/帧1(720p)/333333 |
| 5 | SETUP | 0xA1 | 0x81 GET_CUR | 0x0100 | 0x0001 | 34 | IN: 34B | 设备**回填**：dwMaxVideoFrameSize=460800，dwMaxPayloadTransferSize=1024 |
| 6 | SETUP | 0x21 | 0x01 SET_CUR | 0x0200 | 0x0001 | 34 | OUT: 34B | **COMMIT**：参数敲定生效（VS_COMMIT_CONTROL, CS=2） |

寻址速记：类请求 `wValue = (CS<<8)|0`（CS：1=PROBE，2=COMMIT）；`wIndex = (0<<8)|VS接口号`。VC 控制（如曝光）则是 `wIndex = (实体ID<<8)|VC接口号`，见 [../host/预览与调参.md §3](../host/预览与调参.md) 对照表。

**GET_LEN 是枚举失败高发点**：设备应答 26（UVC1.0）、34（UVC1.1）或 36（UVC1.5）。主机按它解释 Probe 数据长度——固件结构体大小与 GET_LEN 不一致时，Windows 常直接枚举不出摄像头。

### 2.2 开流

```
SETUP: bmRequestType=0x01, bRequest=0x0B, wValue=0x0001(Alt1), wIndex=0x0001, wLength=0
```

这一笔在总线上的真实效果是**带宽仲裁**：主机在当前微帧表中为 EP 0x81 预留每微帧 `wMaxPacketSize` 的同步时间。预留失败（Hub 上同步端点太多）时主机拒绝切挡或直接黑屏——这是"单板直插正常、挂 Hub 掉帧"的第一现场。

---

## 3. 同步 IN 载荷流（基于 UVC 1.5 规范重构，FID 翻转分帧）

前提：Commit 值 `dwMaxPayloadTransferSize=1024`（HS 单事务），一帧 JPEG = 48,000 字节。每包 = 载荷头 + 数据切片，**头计入 1024 预算**。

### 3.1 帧序重构（连续两帧）

| 包 | 载荷前 2 字节 | bmiHeaderInfo 分解 | 数据段 | 说明 |
|---|---|---|---|---|
| 帧A-首包 | `0C 8C` | EOH+SCR+PTS，**FID=0** | PTS(4B)+SCR(6B)+JPEG[0..1011] 共 1012B | 新帧开始：带时间戳 |
| 帧A-中间包 ×46 | `02 80` | EOH，FID=0 | JPEG 切片 1022B | 纯数据 |
| 帧A-尾包 | `02 82` | EOH+**EOF**，FID=0 | 余量（≤1022B） | 帧结束 |
| 帧B-首包 | `0C 8D` | EOH+SCR+PTS，**FID=1** | 时间戳+切片 | **FID 翻转 = 新帧** |
| 帧B-中间包 | `02 81` | EOH，FID=1 | 切片 | … |
| 帧B-尾包 | `02 83` | EOH+EOF，FID=1 | 余量 | … |

标志位图（第 2 字节）：D0=FID，D1=EOF，D2=PTS，D3=SCR，D6=错误位，D7=EOH（恒 1）。

### 3.2 主机怎么"看"这条流

- **分帧**：完全靠 FID 翻转 + EOF。固件丢帧后若不翻转 FID，主机把两帧拼成一帧"无限长帧"→ 缓冲耗尽 → 黑屏；
- **对齐**：PTS（呈现时间戳）+ SCR（源时钟）用于音视频同步——这就是会议设备里 UVC 与 UAC 走不同端点仍能口型对上的时基来源；
- **丢包**：同步传输无重传。抓包里表现为微帧空转或 CRC 错误，主机端帧率下降而非卡死（[../hardware/设计要点.md §7](../hardware/设计要点.md) 的带宽账）；
- 对照固件：这段序列正是 [mjpeg_frame_fill.c](../firmware/src/mjpeg_frame_fill.c) `send_mjpeg_frame()` 吐出的内容。

---

## 4. UAC 录音方向的数据流

- 麦克风（设备→主机，IN 端点）：每毫秒一包，48kHz/16bit/单声道 = 96 字节纯 PCM，**无载荷头**（与 UVC 的本质区别）；
- 本 Lab 教学设备是"录音异步源"：设备按本地时钟发送，**没有反馈端点**——主机自然跟随输入率；
- 主机切换采样率走 EP0 类请求：`SET_CUR(CS=0x01, 端点寻址 0x22/0xA2)` 写 3 字节采样率（对应 [uac_mic_descriptors.c](../firmware/src/uac_mic_descriptors.c) 的 `uac_ep_request()`）。

## 5. UAC 反馈端点数值解读（10.14 / 16.16）

**反馈端点是谁的义务？** 播放方向（主机→设备，OUT）的**异步**设备：设备时钟独立，必须额外声明一个同步 IN 反馈端点，持续告诉主机"我实际吃了多少样本"。典型形态：UVC+UAC 会议耳麦的扬声器通路。抓包看到"神秘 IN 同步端点"，就是它。

### 5.1 全速（FS）：10.14 定点，3 字节，含义 = 每 1ms 帧的样本数

| 场景 | 实际采样率 | 换算（值 = 样本/毫秒 × 2^14） | IN 数据（3B，小端） |
|---|---|---|---|
| 标称精确 48kHz | 48000.000 Hz | 48.000 × 16384 = 786432 = 0x0C0000 | `00 00 0C` |
| 时钟偏快 +100ppm | 48004.800 Hz | 48.0048 × 16384 ≈ 786511 = 0x0C004F | `4F 00 0C` |

解码口诀：**读出 24 位 → 除以 16384 → 每 1ms 的样本数**；小数部分就是时钟漂移的实时补偿量。端点描述符 `bRefresh=3` 表示数值每 2³ 帧更新一次。

### 5.2 高速（HS）：16.16 定点，4 字节，含义 = 每 125µs 微帧的样本数

| 场景 | 换算（值 = 样本/微帧 × 2^16） | IN 数据（4B，小端） |
|---|---|---|
| 标称 48kHz（6 样本/微帧） | 6.000000 × 65536 = 393216 = 0x060000 | `00 00 00 06` |
| +100ppm | 6.0006 × 65536 ≈ 393255 = 0x060027 | `27 00 00 06` |

### 5.3 抓包读法

在 Wireshark 里给反馈端点单独过滤，把连续值画成趋势：围绕标称值（如 0x0C0000）小幅波动 = 健康异步时钟；恒定不变 = 大概率"假异步"（自适应实现硬报固定值）；大幅锯齿 = 缓冲欠载/过载（实战见 [../experience.md §4](../experience.md)）。

---

## 6. 真实抓包指引（把本文的"重构"换成"实拍"）

### 6.1 Windows：USBPcap + Wireshark（零硬件成本）

1. 安装 [USBPcap](https://desowin.org/usbpcap/)，重装/重启 Wireshark；
2. 接好摄像头，Wireshark 捕获接口列表选对应 USB 根集线器（USBPcap 交互式选择设备或全抓）；
3. 过滤器速查：
   - `usb.transfer_type == 0x01`——同步传输（UVC 流/UAC 流）；
   - `usb.transfer_type == 0x02`——控制传输（SET_INTERFACE/PROBE/COMMIT）；
   - `usb.device_address == N`——只看本设备（`lsusb`/设备管理器查地址）；
4. 触发流：开相机应用 → 立即能看到 §2 的类请求与 §3 的 FID 序列；
5. 存档：文件 → 另存为 `captures/lab4-uvc-720p.pcapng`（仓库根目录 captures/）。

### 6.2 Linux：usbmon

```bash
lsusb                                   # Bus 001 Device 005: ... → 总线号 1
sudo modprobe usbmon
wireshark                               # 捕获接口选 usbmon1，过滤语法同上
```

### 6.3 硬件分析仪（商用/疑难杂症）

软件抓包看不到微帧时序、包间隔与实际带宽占用——查"帧率不足""两台摄像头互相挤占"这类问题需要硬件分析仪：

| 工具 | 量级（区间，仅供参考） | 适用 |
|---|---|---|
| TotalPhase Beagle USB 480 | 约 1–2 千美元 | 微帧级同步流分析，本 Lab 场景够用 |
| Ellisys Visual USB / USB Explorer | 数千至上万美元 | 团队级协议回归、认证预测试 |

### 6.4 抓包核对清单（对照固件）

- [ ] PROBE 的 GET_LEN 与固件结构体一致（26/34/36）；
- [ ] COMMIT 后 `dwMaxPayloadTransferSize` 与实际最大包长一致（含载荷头）；
- [ ] FID 序列 0/1 交替，EOF 每帧出现一次；
- [ ] `dwMaxVideoFrameSize` ≥ 实际最大帧；同步端点包长不超微帧预算；
- [ ] UAC 反馈值（如有）围绕标称值波动。

---

## 7. 异常特征 → 根因速查

| 抓包特征 | 最可能根因 |
|---|---|
| SET_INTERFACE(Alt1) 成功但无 IN 数据 | 固件未启流 / 端点未 armed |
| 包长 > dwMaxPayloadTransferSize | Commit 回填与实际打包不一致 → 主机缓冲错位 |
| FID 不翻转、EOF 恒无 | 分帧逻辑 bug → 主机"无限长帧"黑屏 |
| PROBE GET_LEN 与数据阶段不符 | 结构体长度/版本不匹配 → 枚举失败 |
| 播放断续且反馈端点值恒定 | "假异步"时钟（自适应实现） |
| 反馈值大幅锯齿 | 设备缓冲欠载/过载，音频流水线抖动 |

---

## 参考资源

- 知识库：[UVC 详解 §5 载荷传输](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/00-UVC详解.md) · [UVC 规范级附录（状态中断/错误码）](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Video-UVC/01-UVC规范级-控制与格式全表.md) · [UAC1.0 详解 §3 端点与反馈](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/01-UAC1.0详解.md) · [UAC 概述 §4 时钟三模式](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/Audio-UAC/00-UAC概述.md)
- USB-IF 官方文档库（UVC 1.5 / UAC 1.0 规范原文）: https://www.usb.org/documents
- USBPcap（Windows 开源 USB 抓包）: https://desowin.org/usbpcap/
- Wireshark USB 捕获文档（含 usbmon）: https://wiki.wireshark.org/CaptureSetup/USB
- Linux UVC driver & tools: https://www.ideasonboard.org/uvc/

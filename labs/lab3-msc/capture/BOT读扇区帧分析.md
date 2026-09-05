# BOT 读扇区（READ 10）帧分析 —— CBW → 数据 → CSW 逐字段

> ⚠️ **本文帧序列为基于 BOT/SCSI 规范的重构（reconstructed）**，非真实抓包：字节取值按
> *USB Mass Storage Class Bulk-Only Transport Rev.1.0*（BOT-1.0）与 *SCSI SBC-2/UFI* 命令语义逐字段
> 推演，拓扑与数值（Tag、LBA 等）为教学设定。真实抓包操作见第 4 节，建议抓到包后与本文逐字段对拍。
>
> 知识库对照：
> - [MSC 概述与 BOT 传输](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md)（CBW/CSW 字段总表、复位恢复）
> - [SCSI 命令与 UFI](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/01-SCSI命令与UFI.md)、[SCSI 操作码与 Sense 全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/02-SCSI操作码与Sense全表.md)
> - [四种传输类型·批量传输](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)（NAK 反压、DATA0/1 交替——BOT 三段就跑在这层之上）

## 0. 场景设定（重构用的参数）

| 项 | 值 | 对应固件落点 |
|---|---|---|
| 设备 | 本实验 RP2040 MSC 盘，FS 12Mbps，Bulk 端点 0x81(IN)/0x01(OUT)，单包 64B | `usb_descriptors.c` |
| 磁盘 | 1024 扇区 × 512B（READ CAPACITY 报告 LAST LBA = 0x3FF） | `msc_app.c: tud_msc_capacity_cb` |
| 动作 | 主机读 LBA=8 的一个扇区（挂载后读 FAT 表区域的典型动作） | `tud_msc_read10_cb` |
| 序号 | 本条命令 dCBWTag = 0x12345678（主机自增计数，任意值） | 栈内建 |

主机真正发 READ10 之前，枚举后的第一轮对话永远是：
`INQUIRY(0x12)` → `READ CAPACITY(10)(0x25)` → （可选 `TEST UNIT READY(0x00)`）→ 才是第一条 `READ(10)`。
前两条都是"无数据阶段/短数据阶段"的小命令，本文从第一条 READ10 开始逐字节看。

## 1. 事务全景（重构）

```
主机                                   设备(RP2040)
 │ ① CBW 31B  ──────── Bulk OUT ────▶  解析 CDB=READ(10)，准备 512B
 │ ② DATA 512B ─────── Bulk IN  ◀────  512B = 8 × 64B 包（DATA1/DATA0 交替）
 │ ③ CSW 13B  ──────── Bulk IN  ◀────  status=00 成功，residue=0
```

总线上每一段都由若干 IN/OUT 事务拼成：每个事务 = `令牌包 + 数据包(≤64B) + 握手包`，
设备没准备好就 NAK（[批量传输](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)的天然反压）。

## 2. ① CBW（Command Block Wrapper，31 字节）逐字段

**完整字节（重构值）**：

```
55 53 42 43 | 78 56 34 12 | 00 02 00 00 | 80 | 00 | 0A |
28 00 00 00 00 08 00 00 01 00 | 00 00 00 00 00 00
```

| 偏移 | 长度 | 字段 | 本例值 | 解读 |
|---|---|---|---|---|
| 0 | 4 | dCBWSignature | `55 53 42 43` | 固定 "USBC"（小端读出），帮栈从字节流里找回命令边界 |
| 4 | 4 | dCBWTag | `78 56 34 12` | 0x12345678。**配对凭据**：CSW 必须原样带回，栈据此配对命令/状态 |
| 8 | 4 | dCBWDataTransferLength | `00 02 00 00` | 0x200 = **512**：数据阶段总字节数（USB 视角，字节单位） |
| 12 | 1 | bmCBWFlags | `80` | bit7=1 → 数据阶段方向 **IN**（设备→主机）；bit6~0 保留必须为 0 |
| 13 | 1 | bCBWLUN | `00` | LUN 0（单卡座设备恒 0；多槽读卡器在此选槽） |
| 14 | 1 | bCBWCBLength | `0A` | CDB 有效长度 10 字节（其余填 0） |
| 15 | 16 | CBWCB | 见下 | SCSI 命令描述块（**CDB 内部字段是大端**，和包外的小端字段相反，最易踩坑） |

**CBWCB = READ(10) CDB（10 字节）**：

```
28 00 00 00 00 08 | 00 | 00 01 | 00
```

| CDB 偏移 | 字段 | 本例值 | 解读 |
|---|---|---|---|
| 0 | 操作码 | `28` | READ(10)（SBC-2/UFI §4.7；全表见知识库《SCSI 操作码与 Sense 全表》） |
| 1 | 保护/标志 | `00` | RDPROTECT=0：不要求端到端保护 |
| 2~5 | LBA（大端） | `00 00 00 08` | 从 **LBA 8**（第 9 个扇区）开始读 |
| 6 | 保留 | `00` | —— |
| 7~8 | Transfer Length（大端） | `00 01` | 读 **1 块**（块大小来自 READ CAPACITY 的回答 = 512B） |
| 9 | Control | `00` | NACA=0 |

> **两个"长度"的关系（高频面试题）**：`dCBWDataTransferLength=512` 是 USB 数据阶段字节总数；
> CDB 里 `Transfer Length=1` 是介质块数。二者换算靠块大小（1×512=512 ✓）。
> 若两者不一致（主机声明 1024、CDB 只读 1 块），设备应以 CDB 语义为准，并在 CSW 的
> `dCSWDataResidue` 里报告未完成字节数；方向或语义级不符则升级为阶段错误（0x02）。

## 3. ② 数据阶段（512B，8 个 64B IN 包）

设备把 LBA 8 的扇区内容切 8 份发出（每包 DATA1/DATA0 交替，ACK 确认）：

- 固件落点：`tud_msc_read10_cb(lun=0, lba=8, offset=0/64/.../448, buffer, bufsize)` 被栈依次调用；
- 本例盘是新格式化的 FAT 卷，扇区里可见文件系统特征字节（引导扇区是 `EB 3C 90 ... 55 AA`，
  FAT 表区域首字节 `F8 FF FF` 等）——在 Wireshark 里直接展开 "SCSI Data" 即可肉眼对拍；
- 若设备没跟上节奏，对 IN 令牌回 **NAK**，主机原地重试——这就是为什么 SD 卡读慢 200ms 不影响正确性，只影响吞吐。

## 4. ③ CSW（Command Status Wrapper，13 字节）

**完整字节（重构值）**：

```
53 42 53 55 | 78 56 34 12 | 00 00 00 00 | 00
```

| 偏移 | 字段 | 本例值 | 解读 |
|---|---|---|---|
| 0 | dCSWSignature | `53 42 53 55` | 固定 "USBS" |
| 4 | dCSWTag | `78 56 34 12` | **原样回带 CBW Tag**——配对成功 |
| 8 | dCSWDataResidue | `00 00 00 00` | 0：512B 全部传完 |
| 12 | bCSWStatus | `00` | 命令成功 |

至此主机驱动认为"读了 512B"并向文件系统交货。一条 READ10 在 FS USB 上的空场耗时约 1.3ms（重构估算：
3 段 × 事务开销 + 8 数据包），理论吞吐上限 ~1.1 MB/s——这解释了为什么 RP2040 FS 设备上的 SD 卡速度
永远突破不了 1MB/s（详见 host/msc_linux.md 第 5 节）。

## 5. 错误场景：越界 WRITE(10) → REQUEST SENSE（重构）

设定：主机（或上层 bug）对 **LBA=0xFFFFFF** 发 WRITE10，超出 READ CAPACITY 报告的 0x3FF。
这是给固件自测预留的必考场景（`msc_app.c` 里我们主动设这个 Sense）。

**① CBW**（注意 flags=0x00，数据阶段方向 OUT）：

```
55 53 42 43 | 79 56 34 12 | 00 02 00 00 | 00 | 00 | 0A |
2A 00 00 FF FF FF 00 00 01 00 | 00 00 00 00 00 00
```

CDB 解读：`2A`=WRITE(10)，LBA=`00 FF FF FF`（大端），Transfer Length=1 块。

**② 数据阶段：设备直接 STALL Bulk OUT 端点**（命令级错误，一个字节都不收），
主机清 halt 后读 CSW：

**③ CSW**：

```
53 42 53 55 | 79 56 34 12 | 00 02 00 00 | 01
```

- residue = `00 02 00 00` = 512：一个字节都没收下；
- bCSWStatus = `01`：**命令失败**（非 0x02，说明只是这条命令失败，不要求总线复位恢复）。

**④ 主机发 REQUEST SENSE(0x03) 取病因**：

CBW 的 CDB：`03 00 00 00 12 00`（allocation length = 18B）。设备回 18 字节 Sense（本例固件值）：

```
70 00 05 00 00 00 00 0A 00 00 00 00 21 00 00 00 00 00
```

| Sense 偏移 | 值 | 字段/含义 |
|---|---|---|
| 0 | `70` | Current Error，fixed format |
| 2 | `05` | **Sense Key = ILLEGAL REQUEST** |
| 7 | `0A` | 附加长度 10 |
| 12 | `21` | **ASC = LOGICAL BLOCK ADDRESS OUT OF RANGE** |
| 13 | `00` | ASCQ |

对应固件：`tud_msc_read10_cb/write10_cb` 越界分支里的
`tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00)`；
读卡器空槽则是 NOT READY/3Ah/00h（`Medium Not Present`），全表见
[SCSI 操作码与 Sense 全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/02-SCSI操作码与Sense全表.md)。
若 bCSWStatus=0x02（阶段错误），走 BOT 复位恢复三件套：
类请求 `Mass Storage Reset`(0x21/0xFF) → `CLEAR FEATURE(HALT)` 两个端点 → 重发命令（详见知识库 BOT 篇）。

## 6. 抓真实包操作指引（Linux usbmon / Windows USBPcap）

### 6.1 Linux（首选，内核自带）

```bash
sudo modprobe usbmon          # 加载抓包模块
ls /sys/kernel/debug/usb/usbmon | cat   # 或直接看 wireshark 接口列表
wireshark                     # 选 usbmon1（对应设备所在总线，可从 lsusb 的 Bus 号确认）
```

- 显示过滤：`usb.transfer_type == 0x01`（0x01 = Bulk）再叠加 `usb.device_address == 5`；
- Wireshark 自动按 **USB Mass Storage / SCSI** 两层解剖：展开 CBW 可见 `dCBWSignature/dCBWTag/bmCBWFlags/CBWCB`，
  与本文第 2 节表格逐字段对拍；
- 若只显示 `URB_*` 行：Edit→Preferences→Protocols→USB 勾选 "Try to dissect..." 并确认抓的是 usbmon（含 URB 头）。

### 6.2 Windows（USBPcap，开源）

安装 [USBPcap](https://github.com/desowin/usbpcap) 后，Wireshark 接口列表出现 `\\.\USBPcap1`；
勾选目标设备（或全总线）开始抓。过滤与解剖同上；抓包会同时看到控制传输（枚举）与批量传输（BOT）。

### 6.3 没有设备时

Wireshark 官方样例库收录了 USB 抓包样例（含 USB Mass Storage 场景）：
<https://wiki.wireshark.org/SampleCaptures>（页面内搜 "USB"）。样例与本文重构帧对拍，
就是把"重构值"升级成"实测值"的过程。

### 6.4 建议抓的四组实验

| 实验 | 操作 | 应观察到 |
|---|---|---|
| 挂载三连 | 插入设备 | INQUIRY → READ CAPACITY → 若干 READ10（FAT 表/目录） |
| 空读卡器 | SD 后端不插卡 | 周期 TEST UNIT READY + REQUEST SENSE（02/3A/00） |
| 越界读 | 固件临时把 capacity 改小 | READ10 → CSW status=01 → Sense 05/21/00 |
| 安全弹出 | Linux `udisksctl power-off` | SYNCHRONIZE CACHE(0x35) → START STOP UNIT(0x1B) |

## 参考资源

- USB-IF *Mass Storage Class Bulk-Only Transport Rev.1.0*（CBW/CSW 规范原文）— https://www.usb.org/document-library/mass-storage-bulk-only-10
- T10 *SCSI SBC-2（Block Commands）* — https://www.t10.org/cgi-bin/ac.pl?t=f&f=sbc2r16.htm
- 知识库：[00-MSC概述与BOT](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md) · [01-SCSI命令与UFI](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/01-SCSI命令与UFI.md) · [02-SCSI操作码与Sense全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/02-SCSI操作码与Sense全表.md) · [06-四种传输类型](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md)
- Wireshark USB 抓包文档（usbmon/USBPcap）— https://wiki.wireshark.org/CaptureSetup/USB
- usbmon 内核文档 — https://www.kernel.org/doc/html/latest/driver-api/usb/usbmon.html

# 03 · 批量 BOT 读扇区：CBW → DATA512 → CSW 与错误场景（教学样本）

> **⚠️ 诚实声明**：本文包序列为**基于 USB MSC-BOT-1.0、UFI-1.0 与 SBC 命令语义重构的教学样本**，
> 不是真实抓包。CRC 按规范算法实算（[`../tools/_usbcalc.py`](../tools/_usbcalc.py)）；
> dCBWTag 序号、512 字节扇区内容为**示意值**。真实抓包获取见 [README](README.md)
> （Wireshark 样例页 / usbmon·USBPcap 自抓 / USB-IF 合规 trace）。软件抓包可完整看到
> CBW/CSW/数据载荷，但看不到线级 ACK/NAK/STALL 握手包——本文按线级画出，便于理解。

**样本设备**：HS U 盘（批量端点 512 字节），地址 8（**示意值**），
EP1 OUT = 命令/写数据，EP1 IN = 读数据/状态。多字节字段坑位提醒：
**BOT 包头小端，SCSI CDB 大端**——一个包里两种字节序并存。

## 1. 三段式骨架

```
① CBW (31B, 批量OUT) : 命令块包装 —— "去读这个扇区"
② 数据 (批量IN/OUT)  : 本例 512B 读数据, 设备→主机
③ CSW (13B, 批量IN)  : 状态包装 —— "办完了/办砸了"
```

## 2. 正常路径：READ(10)，LBA=1000，1 块

### 2.1 ① CBW（批量 OUT，DATA0）

```text
[SYNC] E1 88 D0      ; OUT 令牌: ADDR=8, ENDP=1（示意地址下的实算值）
[SYNC] C3 55 53 42 43 01 00 00 00 00 02 00 00 80 00 0A
         28 00 00 00 03 E8 00 00 01 00 00 00 00 00 00 00
         42 C1
       │  └──────────────────── 31 字节 CBW ────────────────────┘ └ CRC16(实算)
       └ DATA0
[SYNC] D2            ; 设备 ACK
```

31 字节 CBW 逐字段：

| 偏移 | 字节 | 字段 | 解释 |
|---|---|---|---|
| 0~3 | `55 53 42 43` | dCBWSignature | 小端读出 0x43425355 = "USBC"，CBW 身份牌 |
| 4~7 | `01 00 00 00` | dCBWTag | 命令序号 1（**示意值**，主机逐条递增），CSW 必须原样带回 |
| 8~11 | `00 02 00 00` | dCBWDataTransferLength | 0x200 = 512（小端）——数据阶段期望字节数 |
| 12 | `80` | bmCBWFlags | bit7=1 → 数据 IN（设备→主机）；bit6~0=0 |
| 13 | `00` | bCBWLUN | LUN 0 |
| 14 | `0A` | bCBWCBLength | 命令块有效长 10 字节 |
| 15~30 | 见下 | CBWCB[16] | SCSI CDB，**大端**字段，尾部补 0 |

CBWCB = READ(10) 命令（10 字节有效）：

| CDB 偏移 | 字节 | 字段 | 解释 |
|---|---|---|---|
| 0 | `28` | 操作码 | 0x28 = READ(10) |
| 1 | `00` | 标志 | LUN 高 3 位 + DPO/FUA = 0 |
| 2~5 | `00 00 03 E8` | LBA | 0x000003E8 = **1000**（大端！与包头相反） |
| 6 | `00` | 保留 | 组号 |
| 7~8 | `00 01` | Transfer Length | 1 块（大端；0=不传输） |
| 9 | `00` | Control | — |

### 2.2 ② 数据阶段：512 字节扇区（批量 IN，DATA0）

```text
[SYNC] 69 88 D0      ; IN 令牌（可能先吃几轮 NAK: 设备读闪存要时间）
[SYNC] C3 EB 52 90 4D 53 44 4F 53 35 2E 30 00 02 08 20 00
         (… 其后 496 字节全为 00 …)           7B 83
       │  └── 512 字节扇区数据（示意值: FAT32 引导扇区样式的开头）──┘ └ CRC16(实算)
       └ DATA0 —— 该 IN 端点的第一包, toggle 从 DATA0 起步
[SYNC] D2
```

> 扇区内容（前 16 字节 `EB 52 90 "MSDOS5.0" 00 02 08 20 00`，其余补零）是**构造的示意值**，
> 只为让 CRC 自洽，不是任何真实介质的数据。HS 批量 512B 恰好一包；若经 FS 或更大块数，
> 会看到 DATA0/1 交替的多包序列。

### 2.3 ③ CSW（批量 IN，DATA1 —— toggle 接着数据阶段翻转）

```text
[SYNC] 69 88 D0
[SYNC] 4B 55 53 42 53 01 00 00 00 00 00 00 00 00 54 22
       │  │  ──┬── ────┬──── ─┬─
       │  │    │       │      └ bCSWStatus = 0x00 命令成功
       │  │    │       └ dCSWDataResidue = 0（512 字节如数交付）
       │  │    └ dCSWTag = 0x00000001 —— 与 CBW 配对（小端）
       │  └ dCSWSignature = 0x53425355 = "USBS"
       └ DATA1（CSW 与数据阶段共用 IN 端点, toggle 延续: DATA0→DATA1）
[SYNC] D2
```

主机的核对手则：Tag 配对 → Residue=0 → Status=0x00，三者都对这条命令才算成功。
下一条命令 Tag 递增为 2，IN 端点 toggle 翻到 DATA0。

## 3. 错误场景：读越界 LBA → REQUEST SENSE

让主机读 LBA=0x0FFFFFFF（容量之外），完整还原"命令失败 → 取错误详情"的协议舞步。

### 3.1 第一条命令：READ(10)，Tag=1

```text
[SYNC] E1 88 D0
[SYNC] C3 55 53 42 43 01 00 00 00 00 02 00 00 80 00 0A
         28 00 0F FF FF FF 00 00 01 00 00 00 00 00 00 00   ; LBA=0x0FFFFFFF(大端)
         04 95                                             ; CRC16(实算)
[SYNC] D2            ; CBW 被正常接收 —— 错误在"执行", 不在"接收"

[SYNC] 69 88 D0
[SYNC] 1E            ; 设备 STALL: 拒绝数据阶段（LBA 越界, 无数据可给）
```

### 3.2 主机恢复动作 1：清除 IN 端点挂起

```text
[SYNC] 2D 08 60      ; 控制传输（U 盘 EP0, ADDR=8）—— 视角切到端点 0
[SYNC] C3 02 01 00 00 81 00 00 00 06 D1
         │  │  │     ─┬─
         │  │  │      └ wIndex = 0x0081 → 目标端点 EP1 IN
         │  │  └ wValue = 0 (ENDPOINT_HALT)
         │  └ CLEAR_FEATURE (0x01)
         └ bmRequestType = 0x02: OUT/标准/端点(00010)
[SYNC] D2
[SYNC] 69 08 60      ; 状态阶段: IN
[SYNC] 4B 00 00      ; DATA1 ZLP
[SYNC] D2
```

**规范细节**：CLEAR_FEATURE(ENDPOINT_HALT) 成功后，该端点 toggle **复位为 DATA0**——
所以下一包数据/CSW 又从 DATA0 开始。

### 3.3 主机恢复动作 2：读 CSW（Status=01）

```text
[SYNC] 69 88 D0
[SYNC] C3 55 53 42 53 01 00 00 00 00 00 00 00 01 95 E2
       │  ──┬── ────┬──── ─┬─
       │    │       │      └ bCSWStatus = 0x01 命令失败 → 去 REQUEST SENSE
       │    │       └ dCSWDataResidue = 0
       │    └ dCSWTag = 0x00000001 —— 与 CBW 配对（小端）
       └ DATA0（halt 清除后 toggle 复位, CSW 从 DATA0 重新起步）
[SYNC] D2
```

### 3.4 第二条命令：REQUEST SENSE，Tag=2（错误详情）

```text
[SYNC] E1 88 D0
[SYNC] C3 55 53 42 43 02 00 00 00 12 00 00 00 80 00 06
         03 00 00 00 12 00 00 00 00 00 00 00 00 00 00 00
         AF 15
         │  └ CBWCB = 03 00 00 00 12 00 → REQUEST SENSE, AllocationLength=0x12=18
         └ Tag=2, dCBWDataTransferLength=0x12=18, bmCBWFlags=0x80(IN)
[SYNC] D2

[SYNC] 69 88 D0
[SYNC] 4B 70 00 05 00 00 00 00 0A 00 00 00 00 21 00 00 00 00 00 70 FA
       │  └────── 18 字节 Sense 数据 ──────────────────────┘ └ CRC16(实算)
       └ DATA1（CSW 用了 DATA0, toggle 正常翻到 DATA1）
[SYNC] D2
```

18 字节 Sense（SPC 风格布局）逐字段：

| 偏移 | 值 | 字段 | 解释 |
|---|---|---|---|
| 0 | `70` | Error Code | 0x70 = 当前错误；bit7 Valid=0（Information 无效） |
| 2 | `05` | Sense Key | **ILLEGAL REQUEST**：命令参数非法 |
| 7 | `0A` | Additional Sense Length | 后面还有 10 字节 |
| 12 | `21` | **ASC** | LBA OUT OF RANGE——正是"越界"的官方诊断 |
| 13 | `00` | ASCQ | 限定符 |
| 其余 | `00` | 保留/Information/Cmd-specific | — |

> **类规范差异坑**：UFI 设备（子类 0x04，软盘/读卡器）的 Sense 布局按 UFI Table 39，
> **ASC/ASCQ 在偏移 11/12**、ASL 在偏移 8（同值时为 `70 00 05 00 00 00 00 00 0A 00 00 21 00 …`）；
> 而 SBC U 盘（子类 0x06）遵 SPC，ASC/ASCQ 在 **12/13**。解析 Sense 前先看设备子类。
> Sense 数据被 REQUEST SENSE 取走后 Check Condition 自清——不取回会被下一条命令覆盖。

```text
[SYNC] 69 88 D0
[SYNC] C3 55 53 42 53 02 00 00 00 00 00 00 00 00 40 D2
                                              ─┬─
                                               └ Status=0x00（取 Sense 这条本身成功）
[SYNC] D2            ; 随后主机重试原读请求 / 上抛 I/O 错误, 视文件系统策略
```

## 4. CSW 状态码与恢复路径总表

| bCSWStatus | 含义 | 主机动作 |
|---|---|---|
| 0x00 | 成功 | 继续下一条命令 |
| 0x01 | 命令失败 | 发 REQUEST SENSE 取 ASC/ASCQ → 清 halt → 重试或报错 |
| 0x02 | **阶段错误** | 主机声明的方向/长度与命令语义不符 → 只能 Bulk-Only Reset（类请求 0x21/0xFF）+ 双端点 ClearHalt + 从头再来 |

## 5. Wireshark 实战对照

软件抓包能完整看到三段与 Sense 内容（握手包不可见）：

```
ums.dCBWSignature == 0x43425355     ; 只看 CBW
ums.bCSWStatus == 0x01              ; 只看失败的 CSW
scsi.opcode == 0x28                 ; READ(10) 命令
scsi_sbc.rdwr10.lba                 ; 提取 LBA 列
scsi.sense.key == 0x05              ; ILLEGAL REQUEST
```

更多过滤器见 [`../tools/wireshark_filters.md`](../tools/wireshark_filters.md)。

## 参考资源

- USB Mass Storage Class Bulk-Only Transport (BOT) Rev 1.0: https://www.usb.org/documents?search=&type%5B0%5D=55
- USB Mass Storage Class UFI Command Set Rev 1.0（Table 25/38/39/50-53）
- SCSI SBC/SPC 命令与 Sense 语义: https://www.t10.org
- 知识库对照: USBTree `20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md`、`01-SCSI命令与UFI.md`、`02-SCSI操作码与Sense全表.md`
- 真实 U 盘抓包: https://wiki.wireshark.org/SampleCaptures （搜 "USBMS" / "USB Mass Storage"）
- 本系列: [02-中断IN轮询.md](02-中断IN轮询.md) · [04-同步流与反馈.md](04-同步流与反馈.md)

# Lab6 抓包分析 · BLE 连接建立序列（键盘配对全流程）

> ⚠️ **诚实声明**：以下帧序列为**基于 Bluetooth Core Spec（v5.x）重构的教学样本**，
> 字段取值符合规范定义，时间戳与句柄为教学编造，**不是真实采集数据**。
> 真实抓包方法：[../host/btmon.md](../host/btmon.md)（nRF Sniffer + Wireshark / btmon / Android HCI 日志）。
> 协议背景：知识库 [03-广播与连接](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/03-广播与连接.md)、
> [04-ATT与GATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/04-ATT与GATT.md)、
> [06-SMP安全与配对](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/06-SMP安全与配对.md)、
> [12-LLCP控制过程全表](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/12-LLCP控制过程全表.md)。

## 0. 场景设定（重构样本参数）

- 外设 P（键盘）：随机静态地址 `F2:9A:xx:xx:xx:xx`（高两位=11），广播名 "LAB6-KB"，广播间隔 20ms。
- 中央 C（PC/笔记本蓝牙适配器）。
- IO 能力：P = NoInputNoOutput，C = KeyboardDisplay → 走 **LE Secure Connections Just Works**。
- 时序列：`t+` 为相对连接完成事件，非真实设备测量值。

## 1. 广播与连接（链路层）

| # | PDU | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 1 | **ADV_IND**（ch=37） | AdvA=F2:9A:…, Flags(LE General Disc+BR/EDR Not Supported), 名称"LAB6-KB", AD: 0x1812 完整16bitUUID列表, Appearance=Keyboard | 在 37/38/39 三信道轮发；HOGP 设备惯例把 0x1812（HID）放广播数据，便于主机秒筛 |
| 2 | ADV_IND（ch=38） | 同上 | 跳下一广播信道 |
| 3 | ADV_IND（ch=39） | 同上 | 主机在此信道收到并决定连接 |
| 4 | **CONNECT_IND**（C→P，ch=39） | InitA=C, AdvA=P；LLData：**AA**=0x8e89bed6（教学值），CRCInit=0x555555，**WinSize=0x02（1.25ms）**，WinOffset=0x0008，**Interval=0x0018=37.5ms**，Latency=0x0004，Timeout=0x012C（3000ms），ChM=0x1FFFFFFFFF（全37数据信道），**Hop=9**，SCA=5 | 建立连接的核心 PDU：接入地址/跳频增量/信道图/初始时序全部在此一次定死。Latency=4 表示外设可跳过 4 个连接事件打盹 |

## 2. 链路层特性协商（LLCP）

| # | PDU | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 5 | LL_VERSION_IND（P→C） | Version=0x0A (5.0+)，CompId=0x0059（教学值） | 双方版本交换，只发一次 |
| 6 | LL_VERSION_IND（C→P） | Version=0x0C | 同上 |
| 7 | **LL_LENGTH_REQ/RSP** | MaxRxOctets=251, MaxTxOctets=251 | 数据长度扩展（DLE），单包多于 27 字节；HID 报告用不满，但协商无害 |
| 8 | **LL_PHY_REQ/RSP → LL_PHY_UPDATE_IND** | P/C 均请求 2M；IND: M→S=2M, S→M=2M | 切 2M PHY，缩每包空中时间；对 HID 握手延迟是纯收益 |

> LLCP 控制过程（LLCP）全表见知识库 [12-LLCP控制过程全表](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/12-LLCP控制过程全表.md)。

## 3. 连接参数更新（L2CAP）

| # | 报文 | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 9 | L2CAP Connection Parameter Update Request（P→C，信令 CID 0x0005，code=0x12） | interval min/max = 30/50ms，latency=4，timeout=3000ms | 外设按功耗诉求提参：先粗后细 |
| 10 | L2CAP Connection Parameter Update Response（code=0x13） | Result=0x0000 | 主机接受，随后以 LL_CONNECTION_UPDATE_IND 落地（此处略） |

## 4. SMP 配对与加密

| # | 报文 | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 11 | **SMP Pairing Request**（P→C，L2CAP CID 0x0006，cmd=0x01） | IOCap=0x03(NoInputNoOutput)，OOB=0，**SC=1**，Bonding=1，MITM=0，MaxKeySize=16，InitKdist/RespKdist：Enc+Id | NoInputNoOutput + MITM=0 ⇒ Just Works；Bonding=1 ⇒ 双方存密钥 |
| 12 | SMP Pairing Response（C→P，cmd=0x02） | 对称字段，SC=1 | 双方确认走 LE Secure Connections（ECDH P-256） |
| 13 | Pairing Public Key（0x0C）×2 | PKax/PKax 64 字节×2 | 交换 ECDH 公钥 |
| 14 | Pairing Confirm（0x03）×2 | Confirm 值 16B | DHKey Check 前的确认计算 |
| 15 | Pairing Random（0x04）×2 | Nonce 16B | 同上 |
| 16 | Pairing DHKey Check（0x0D）×2 | Ea/Eb 16B | 防 MITM 校验（Just Works 下无认证属性） |
| 17 | **HCI LE Start Encryption / LL_ENC_REQ…LL_START_ENC_REQ/RSP** | LTK=…（16B） | 分发加密密钥并启动链路加密；此后空口报文对 Sniffer 而言加密，需 LTK 才能解密 |
| 18 | Identity Address Information（0x09）/ IRK（0x08）（C→P） | C 的身份地址 | 隐私地址解析用；键盘无 RPA 时此步可简化 |

> 配对方法选择的完整决策树见知识库 [06-SMP安全与配对](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/06-SMP安全与配对.md)。

## 5. GATT 服务发现（应用层，ATT）

| # | 报文 | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 19 | ATT Exchange MTU Request/Response（0x02/0x03） | MTU=247 | 配对后主机例行调大 MTU |
| 20 | **Read By Group Type Req**（0x10，type=Primary Service 0x2800） | — | 主机枚举主服务 |
| 21 | Rsp（0x11） | [0x1800 GAP][0x1801 GATT][0x180A DeviceInfo][**0x180F Battery**][**0x1812 HID**] | 五个主服务；0x1812 就是 HOGP |
| 22 | Read By Type Req（0x08，type=Characteristic 0x2803） | 范围=0x1812 服务句柄区间 | 枚举 HID 服务内特征值 |
| 23 | Rsp（0x11） | 含：HID Information(**0x2A4A**)、Report Map(**0x2A4B**)、Report(**0x2A4D**)×N、HID Control Point(0x2A4C)、Protocol Mode(0x2A4E)、Boot Keyboard Input(0x2A22) | 每个特征值带句柄、属性、UUID |
| 24 | **Read Request（0x04）→ Report Map** | 返回 = HID 报告描述符全部字节 | ★ **主机从这里拿到与 USB 侧同一份描述符**（本实验室复用核心，见 README §2） |
| 25 | Read By Type Req（type=Report Reference 0x2908） | — | 查每个 Report 的 (Report ID, 类型) |
| 26 | Find Information Req（0x04） | — | 枚举 CCCD（0x2902）句柄，为订阅做准备 |
| 27 | GATT Write（0x52）→ **CCCD[键盘 Input Report]** | 值=0x0001 | **使能 Notify：这一步完成，键盘才算"能打字"** |
| 28 | Write（0x52）→ CCCD[Battery Level 0x2A19] | 值=0x0001 | 订阅电量通知 |
| 29 | Read（0x04）Battery Level | 值=0x5A（90%） | 系统读当前电量 |

## 6. 稳态：HID Notification

| # | 报文 | 关键字段（重构值） | 解读 |
|---|---|---|---|
| 30 | ATT Handle Value Notification（0x1B） | Handle=0x0015（键盘 Input Report），值=`01 00 00 04 00 00 00 00` | 报告 ID=01，字节3=0x04 即 "a" 键按下（HID Usage ID 0x04），与 USB 报告格式完全一致 |
| 31 | Notification | 值=`01 00 00 00 00 00 00 00` | 松键全零报告（HID 语义：无按下键也要上报） |

## 7. 重连序列（第二天开机）

```
P: ADV_DIRECT_IND(低占空比, AdvA=P, InitA=C)   ← 只想连白名单里这台主机
C: CONNECT_IND                                  ← 主机秒应答
P/C: LL_START_ENC_REQ/RSP（用已存 LTK，无重新配对）  ← bond 的意义
C: Write CCCD 或直接沿用绑定保存的订阅状态
P: HID Notification……
```

要点：重连**跳过配对与 GATT 发现**（GATT 缓存 + CCCD 随 bond 持久化），用户感知就是"秒连"。这也是 [../experience.md](../experience.md) §1 里定向广播体验设计的协议基础。

## 8. Wireshark 对照过滤式

```
广播:     btcommon.eir_ad.entry.device_name contains "LAB6-KB"
连接事件: bthci_evt.code == 0x3e && bthci_evt.le_meta_subevent == 0x01
配对:     btsmp.opcode
GATT发现: btatt.opcode == 0x10 || btatt.opcode == 0x08
HID报告:  btatt.opcode == 0x1b
```

## 参考资源

- Bluetooth Core Spec（SIG 官方，本文件全部字段定义来源）：https://www.bluetooth.com/specifications/specs/
- Bluetooth Assigned Numbers（UUID/ATT opcode/HD 码表）：https://www.bluetooth.com/specifications/assigned-numbers/
- HOGP（HID over GATT）规范：https://www.bluetooth.com/specifications/specs/hid-over-gatt-1-0/
- Wireshark 样例抓包库（公开真实 BLE 样例参照）：https://wiki.wireshark.org/SampleCaptures
- 知识库：[00-BLE概述](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/00-BLE概述.md) · [03-广播与连接](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/03-广播与连接.md) · [04-ATT与GATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/04-ATT与GATT.md) · [07-HOGP-HIDoverGATT](https://github.com/tangjianfang/USBTree/blob/main/50-枝干-无线关联/BLE-低功耗蓝牙/07-HOGP-HIDoverGATT.md)

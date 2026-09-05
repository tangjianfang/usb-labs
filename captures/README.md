# captures/ — USB 抓包分析系列

> **⚠️ 诚实声明（先读这一段）**
> 本目录下所有 `.md` 文件中的帧/事务/包序列，都是**基于官方规范重构的教学样本**，
> **不是**真实设备抓包记录。它们回答"规范的字节应该长什么样"，不能替代真实 trace 中
> 的时序抖动、重试、位填充与设备 quirks。
> 文中字节满足两条纪律：
> 1. 结构性字段（PID、请求、描述符、CBW/CSW、Sense、PD Header/PDO/RDO）严格按官方规范取值；
> 2. CRC5/CRC16 按规范算法（USB 2.0 §8.3.5：移位寄存器全 1 种子、余数取反、MSb 先发）对文中负载**实算**得出，
>    生成脚本见 [`tools/_usbcalc.py`](../tools/_usbcalc.py)（内嵌规范余数校验 01100B / 1000000000001101B 与 CRC-16/USB 目录校验值 0xB4C8 双重断言）。
> 设备身份（VID/PID、字符串）、采样数据、帧号、电流电压档位等**内容性**字节均标注为"示意值"。

## 真实抓包怎么来？三条路

真实 trace 才是最终事实。按成本从零到高：

### 1. 下载别人抓好的（0 元）

- Wireshark 官方样例捕获页（大量社区贡献的 USB pcap/pcapng）：
  **https://wiki.wireshark.org/SampleCaptures** （搜索 "USB"）
- USB-IF 合规测试 trace / USB-IF 发布的 xHCI、CV 测试记录（部分需会员权限）；
- 各分析仪厂商官网的 demo capture（Beagle/Ellisys 均提供样例文件）。

### 2. 自己抓：软件层（0 元，90% 场景够用）

**Linux usbmon**（详见 [`../tools/usbmon_howto.md`](../tools/usbmon_howto.md)）：

```bash
sudo modprobe usbmon                      # 加载模块（多数发行版已内置编译）
ls /dev/usbmon*                           # usbmonN 对应总线 N，usbmon0 = 全部
lsusb                                     # 找到目标设备的总线号/设备号
sudo tshark -i usbmon2 -w enum.pcapng     # 命令行抓总线 2
sudo wireshark                           # 或图形界面直接选 usbmon2 接口
```

**Windows USBPcap**：

1. 从 https://desowin.org/usbpcap/ 安装（自带 Wireshark 插件），**装完重启 Wireshark**；
2. 接口列表出现 `USBPcap1`（每个根控制器一个）；
3. 选中接口开始捕获；想只抓某端口，先运行 `USBPcapCMD` 查看设备树选择过滤。

注意：软件抓包是 **URB 级**（submit/complete 两条记录），线级 NAK/重试/位填充不可见——需要线级细节时上硬件。

### 3. 自己抓：硬件层（花钱买真相）

| 梯队 | 代表工具 | 能看到 | 价位 |
|---|---|---|---|
| 逻辑分析仪 | Saleae Logic、数十元 24MHz 克隆 | D+/D- 电平、速率、复位/Chirp 波形；粗粒度包 | 数百元 |
| FS/HS 协议分析仪 | Total Phase **Beagle USB 12 / 480** | 线级全包（含 NAK/重试）、实时解码 | 数千~数万元 |
| 企业级 | **Ellisys** Visual USB、Teledyne LeCroy | 全速~SuperSpeedPlus、触发、协议专家系统 | 数万~数十万元 |
| 开源 | **OpenVizsla**（FPGA） | FS/HS 线级捕获 | 数千元 |
| Type-C/PD | Total Phase PD 分析仪、GRL、Twinkie（开源） | CC 线 BMC 报文、VBUS 曲线 | 数千~数万元 |

选型纪律：**先软件后硬件**——枚举/描述符/类协议问题软件层即可定位；电气与时序问题才需要波形和线级包。

## 文件索引

| 文件 | 内容 | 对应规范 |
|---|---|---|
| [00-全速枚举逐包标注.md](00-全速枚举逐包标注.md) | FS 鼠标从插入到 SET_CONFIGURATION 的完整包序列，逐字节标注 | USB 2.0 第 8/9 章 |
| [01-控制传输三阶段.md](01-控制传输三阶段.md) | 以 SET_CONFIGURATION 讲透 Setup/Data/Status 与 DATA0/DATA1 规则 | USB 2.0 §5.5/§8.5.3 |
| [02-中断IN轮询.md](02-中断IN轮询.md) | HID 鼠标 IN 事务、NAK 场景与 bInterval 节拍 | USB 2.0 §5.7 + HID 1.11 |
| [03-批量BOT读扇区.md](03-批量BOT读扇区.md) | U 盘 CBW(READ10)→DATA512→CSW + REQUEST SENSE 错误场景 | USB 2.0 §5.8 + MSC-BOT/UFI |
| [04-同步流与反馈.md](04-同步流与反馈.md) | UAC 异步播放同步 OUT + 10.14 反馈 IN | USB 2.0 §5.6 + UAC 1.0 |
| [05-PD协商序列.md](05-PD协商序列.md) | PD 消息级协商：Source_Capabilities→Request→Accept→PS_RDY | USB PD 3.x（消息级重构） |

## 阅读约定（适用于全部样本）

- **SYNC**：每包 8 位同步字段（FS 线上呈 KJKJKJKK，即位流 `00000001`），不在字节流中逐字节展开，仅以 `[SYNC]` 标注；
- **EOP**：SE0×2 + J，不再赘述；
- **PID 字节**：低 4 位类型 + 高 4 位取反，如 IN=`0x69`、SETUP=`0x2D`、OUT=`0xE1`、SOF=`0xA5`、DATA0=`0xC3`、DATA1=`0x4B`、ACK=`0xD2`、NAK=`0x5A`、STALL=`0x1E`；
- **CRC**：token 的 CRC5 覆盖 ADDR+ENDP（11 位），data 的 CRC16 覆盖全部负载；均按规范"取反后 MSb 先发"实算并折回字节流（CRC16 低反射字节在前）；未考虑位填充插入点（真实线位流中 `011111` 后会插 0，分析线级波形时注意）；
- **示意值**：所有随设备/内容变化的字节（VID/PID、字符串、采样数据、扇区数据、帧号、电压电流档位等）。凡未标"示意值"的字段，均可在对应规范的表中直接查到；
- **样本设备**：00/01/02 为同一只"USBLabs 双协议鼠标"（FS、HID、Boot+Report 双协议、EP1 IN 中断 1ms），03 为 HS U 盘，04 为 FS UAC1 异步声卡，05 为 PD 65W 充电器 + 手机——跨文件的描述符/报告字节互相自洽。

## 参考资源

- USB 2.0 Specification（§8.3.5 CRC、第 8 章包格式、第 9 章枚举）: https://www.usb.org/documents?search=&type%5B0%5D=55&items_per_page=50
- Wireshark 样例捕获页（真实 USB trace 下载）: https://wiki.wireshark.org/SampleCaptures
- USBPcap（Windows 软件抓包）: https://desowin.org/usbpcap/
- Linux usbmon 文档（内核源码 Documentation/usb/usbmon.txt）: https://www.kernel.org/doc/html/latest/usb/usbmon.html
- USB-IF 合规测试页: https://www.usb.org/compliance
- 本系列规范参照知识库: `10-树干-USB核心/05-包格式与事务.md`、`07-描述符详解.md`、`08-枚举流程与标准请求.md`（USBTree 仓库）

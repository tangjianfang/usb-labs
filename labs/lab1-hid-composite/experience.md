# Lab1 商业实战经验：从"能枚举"到"能量产"

> 定位：面向量产工程师的真实项目经验，按"公开资料共识 + 项目可验证方法"组织。
> 涉及价格的均为**区间并随行情浮动**；涉及规范数值的标注出处，不确定处标"以规范/官方文档为准"。
> 关联：[README](README.md) · [BOM](hardware/BOM.csv) · [抓包分析](capture/HID帧序列分析.md)

## 1. 回报率 125Hz → 1000Hz：实现与营销

### 实现本质

轮询速率由**两个数字共同决定**，任一不符就不成立：

1. **设备端**：端点描述符 `bInterval`。Full-Speed 单位为帧（1ms）：`1=1000Hz`、`2=500Hz`、`4=250Hz`、`8=125Hz`。
   本 Lab 固件即 `bInterval=1`（见 `firmware/src/usb_descriptors.c`）。
2. **主机端**：操作系统调度器尊重 bInterval 保证"不低于该周期轮询"，但实际节拍受主机负载影响；
   验证方法是抓包统计 IN 事务间隔（见 `capture/` §3.3 的 IO Graph 法），**用数据说话，不靠感觉**。

同时，"1000Hz 报告"要求固件真的有 1kHz 的新数据可发（传感器/键阵扫描 ≥1kHz），否则主机轮询 1000 次、设备 NAK 999 次——
抓包里满屏 NAK，宣传页面上的数字经不起软件实测，这就是虚标。

### 带宽账（量产必须算）

FS 中断端点 wMaxPacketSize=16B 时，一次完整事务（令牌+数据+握手+包间隔）约 20μs 量级；
本设备 3 个端点同 1ms 轮询，合计占用约 6% 帧带宽，单口独立使用毫无压力。但接入低速集线器/带宽紧张的口时
要重新核算——量产测试矩阵里必须包含"经廉价 HUB + 6 个外设同插"的场景。

### 营销话术的诚实边界

- 可写："硬件 1000Hz 轮询（bInterval=1ms）"，附抓包/工具实测截图；
- 不该写："延迟降低 8 倍"——从 125Hz 到 1000Hz，**轮询环节**平均等待从 4ms 降到 0.5ms，
  但端到端延迟还包括扫描、无线传输（若无线）、主机处理，整机提升幅度以整机实测为准；
- 公开资料共识：竞技场景下人手/屏幕链路更敏感于回报率稳定性（抖动）而非单纯峰值，
  稳定 1000Hz 比忽快忽慢更有感知。

## 2. NKRO（全键无冲）

- **6KRO 限制**来自 boot 协议键盘报告的 6 键码槽，不是 USB 总线限制。
- **实现 NKRO**：报告描述符追加一个位图报告（如 16 字节 × 8bit = 128 键），独立 Report ID；
  主机走报告协议模式时读位图，BIOS 场景仍回退 6 键 boot 报告——两种报告**并存于同一接口**是主流做法（公开资料共识）。
- **概念澄清**："鬼键（ghosting）"源于键阵无二极管的电流旁路，属**硬件矩阵问题**；MCU 端固件再正确也无法修复。
  键盘 OEM 的检查单：每键串联二极管（或商家宣称的"全键二极管"）、扫描时序防抖、NKRO 位图报告。
  参考：https://deskthority.net/wiki/Rollover （社区百科，公开资料共识）。
- 注意 macOS/Linux 对自定义 Usage Page 的 NKRO 位图支持良好，但部分 BIOS 只认 boot 键盘——所以 boot 兼容报告不能省。

## 3. 休眠功耗（<10μA 目标）

### 规范侧

USB 2.0 对挂起状态的设备电流有明确限值（高功率已配置设备挂起电流 2.5mA 量级，详见规范 §7.2.3，**以规范为准**）；
"<10μA"是**产品自设指标**，不是 USB 规范要求，主要用于无线接收器"电脑关机后几乎不耗电"的销售卖点。

### 工程侧（诚实版）

- **RP2040 不是低功耗芯片**：其 sleep 模式仍在 mA 量级，dormant 模式可达 10μA 量级但唤醒后需重建时钟、
  USB 外设状态全部丢失。做 <10μA 待机，公开资料共识的量产做法是：
  - 接收器采用低功耗 MCU（CH55x/CH32V 低功耗型号/nRF52 等）或
  - 硬件负载开关彻底切断主 SoC 供电，仅留唤醒检测电路。
- **实测方法**（本项目可复现）：
  1. 电流表/μCurrent Gold（µA 分辨率）串入 VBUS；或 USB 功率计（注意 1mA 以下分辨率不够）；
  2. 主机进入挂起（Linux: `echo -n 0000:00:14.0 > /sys/bus/pci/drivers/.../power/...` 或直接睡眠整机），读稳态电流；
  3. 分别记录：已配置挂起 / 未配置挂起 / 遥控唤醒瞬间峰值（峰值不得超过端口限流）。
- **遥控唤醒（Remote Wakeup）**：需要设备描述符 `bmAttributes` bit5 置位 + 固件在挂起中发 RESUME 信号；
  TinyUSB 有 `tud_suspend_cb`/`tud_remote_wakeup()`（以官方 API 为准）。注意 macOS 默认策略与 Windows 电源管理
  勾选项（设备管理器"允许此设备唤醒计算机"）对用户可见行为的影响。

## 4. Windows / macOS / Android 三端兼容坑

| 坑 | 现象 | 根因与对策 |
|---|---|---|
| **boot 协议回退** | BIOS/快速启动阶段键鼠失灵，进系统正常 | 主机在 BIOS 阶段用 boot 协议（SET_PROTOCOL）。键盘/鼠标接口必须声明 boot 子类+协议（本固件已做），且 boot 报告格式固定 8/4 字节。消费控制接口无 boot 语义，BIOS 下媒体键失灵属正常 |
| **Windows 描述符校验** | "USB 设备无法识别"/代码 10 | wTotalLength 算错、HID 描述符 wDescriptorLength 与实际报告描述符长度不符。对策：把描述符逐字节对回抓包（见 capture/ §1.1） |
| **Windows 排他打开** | 自研配置工具打不开接口 | Windows 对键盘/鼠标类顶层集合有系统级独占。量产方案：加一个自定义接口（Vendor 类）做配置通道，避免与 HID 输入打架 |
| **macOS 消费控制** | 部分媒体键无效 | macOS 对 Consumer Page 标准 Usage（0xE2/0xE9/0xEA/0xCD）支持良好，但对私有 Usage 静默忽略。**只用官方 Usage Tables 里的标准 Usage** |
| **Android** | 复合设备只出鼠标不出键盘等怪象 | Android HID 主机栈对 Report ID 与描述符结构较敏感；公开资料共识是复合 HID 每个接口带独立报告描述符 + Report ID 最稳（本固件即如此）。以官方文档为准：https://source.android.com/docs/core/interaction/input/keyboard-device |
| **Linux** | 一般最宽松 | hid-generic 兜底；自定义报告需要 hidraw 才能读到原始流（本 Lab 用 hidapi 已跨平台封装） |

## 5. 按键寿命测试（万次级起步）

量产键盘/接收器的输入器件按"百万次"级考核（主流机械轴公开标称 5000 万次量级，薄膜键 100 万~1000 万次量级，
轻触开关 10 万~100 万次量级——均以厂商规格书为准），与"万次级"快速验证测试是两回事：

- **快速验证（研发/来料）**：电动推杆或凸轮治具，以 3~5 次/秒敲击 1 万~10 万次，每 5000 次记录一组接触阻抗与抖动波形；
- **判定标准示例**：接触阻抗增幅 <50%、抖动时长不超标（防连击）、无卡键；序列号 + 测试数据入库，可追溯；
- **固件配合**：本 Lab 的 `hid_monitor.py` 可改造为计数器（统计按键报告次数），配合治具自动出报告；
- **顺带测连接器**：Type-C 插拔按规格 10000 次量级（以 USB 连接器规范为准），抽测插拔后枚举成功率必须 100%。

## 6. 量产烧录与产线

| 阶段 | 方法 | 说明 |
|---|---|---|
| 研发 | BOOTSEL 拖 uf2 | 零工具，见 `firmware/README.md` |
| 小批量 | SWD 探针（pogo pin 治具）+ picotool/openocd | 秒级烧录 + 读回校验；`picotool`: https://github.com/raspberrypi/picotool |
| 量产 | 产线治具一次性完成：烧录 + 写序列号 + 功能测试（枚举 + 按键扫描 + 电流） | 序列号建议烧到独立 Flash 区/OTP，开机由固件读出填 USB 序列号描述符（本固件用 pico_unique_id 读 Flash 唯一 ID 即此思路） |

- **RP2040 的 OTP**：片内有 OTP 区可存少量关键数据（用途与烧录方式以 RP2040 Datasheet 为准）；
  CH552/CH32 则用片内 Flash/OTP，出厂预烧后锁定（以 WCH 手册为准）。
- **免驱动验证工位**：产线电脑装好驱动白名单（HID 天生免驱动），工位脚本 `lsusb`/`pnputil` 确认 VID/PID/序列号，
  与 MES 系统绑定——防"贴错标/烧错版"。

## 7. 售后：一半的"驱动问题"其实是描述符问题

真实客诉三连（公开资料与社区共识的高频项）：

1. **"我的电脑说找不到驱动"** → 九成是枚举失败：描述符非法（wTotalLength 错、报告描述符 Item 错）、
   供电不足（bMaxPower 宣告与实测不符被端口限流）、线材劣质。先抓包看枚举停在哪一步（capture/ §3），再谈驱动。
2. **"键盘会乱码/漏键"** → 报告描述符位段与固件发送数据不一致（Logical Max、报告长度），或键阵硬件鬼键（见 §2）。
3. **"换台电脑就不行"** → VID/PID 被某驱动程序占用（冒用 VID 的经典恶果）、或主机电源管理把口关了
   （Windows USB selective suspend）。对策：自有 VID/PID + 驱动绑定白名单测试。

排障 SOP：**先抓包（客观），再查描述符（对照源码），最后才怀疑主机**——顺序反了会浪费数倍工时。
可复用知识库排查手册：[02-枚举失败排查手册](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/02-枚举失败排查手册.md)。

## 8. 认证与合规（上市前清单）

- **CE（欧盟）**：EMC 指令 2014/30/EU（EN 55032/EN 55035 类标准）+ RoHS 2011/65/EU；
- **FCC（美国）**：Part 15B 无意辐射（数字设备），HID 外设一般走 SDoC 路线；
- **USB-IF**：非强制，但商标使用/大客户验厂常要求合规测试（USB 2.0 Electrical/Interoperability），
  且**必须拥有合法 VID**，见 https://www.usb.org/getting-vendor-id ；
- 认证费用量级（随行情/实验室浮动）：小型 EMC 实验室全项 CE/FCC 报价数千至数万元人民币区间，多询价三家；
  测试项解读见知识库 [03-USB-IF合规认证](https://github.com/tangjianfang/USBTree/blob/main/70-枝干-调试测试与安全/03-USB-IF合规认证.md)。
- 本 Lab 的 BOM 中 ESD/共模电感选型即为此铺路：认证整改时"贴/不贴"的跳线空间要在第一次打版就留好。

## 参考资源

- USB 2.0 规范（§7 电源/§9 枚举）：https://www.usb.org/document-library/usb-20-specification
- HID 1.11 与 Usage Tables：https://www.usb.org/document-library/device-class-definition-hid-111 · https://usb.org/hid
- USB-IF Vendor ID 申请：https://www.usb.org/getting-vendor-id
- TinyUSB（suspend/remote wakeup API 以源码 hid_device.h/usbd.h 为准）：https://github.com/hathach/tinyusb
- RP2040 Datasheet（OTP/功耗/时钟）：https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf
- Rollover/NKRO（deskthority 社区百科，公开资料共识）：https://deskthority.net/wiki/Rollover
- Microsoft HID 架构文档：https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/
- Android 输入设备文档：https://source.android.com/docs/core/interaction/input/keyboard-device
- 知识库：[10-电源管理与挂起唤醒](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/10-电源管理与挂起唤醒.md) ·
  [05-键盘详解](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/05-键盘详解.md) ·
  [07-消费控制与多媒体](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/HID-人机接口设备/07-消费控制与多媒体.md)

# Lab2 工程经验簿（experience.md）

> 定位：Lab2 五个"文档不写、现场天天遇到"的经验专题。社区经验均标注为**公开资料共识**并附真实链接；价格区间随行情。
> 相关：[hardware/设计要点.md](hardware/设计要点.md) · [firmware/src/bootloader选型与跳转设计.md](firmware/src/bootloader选型与跳转设计.md) · [host/dfu_guide.md](host/dfu_guide.md)

## 1. Windows COM 口号漂移与 inf / USB 序列号固定

### 1.1 现象与机理

客户投诉的经典描述："昨天还是 COM3，今天插上变成 COM15，我的 MES 软件找不到了。"

机理（公开资料共识）：Windows 按 **VID + PID + 序列号（iSerialNumber）** 三元组为设备在注册表里记忆配置。三元组全对上 → 复用记忆的 COM 号；任何一项变化（尤其是序列号）→ 被"新设备"对待，串口仲裁器分配新 COM 号。于是：

- **换 USB 口/换 hub 就变号**：多半是设备没烧唯一序列号（iSerialNumber=0），Windows 只好按"端口位置"区分，插哪儿算哪儿；
- **两台同型号设备互相"抢号"**：量产时偷懒烧了同一个序列号，两台设备同时插必然冲突；
- **刷过一次固件后变号**：固件升级改了 VID/PID 或序列号生成逻辑（典型：无序列号 → 有序列号的固件迭代）。

Windows 10+ 对 CDC-ACM 用内置 `usbser.sys` 免驱（官方支持矩阵见 [Microsoft 支持的 USB 类](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes)）；USB 设备在注册表的记忆位置与细节见 [USB device-specific registry settings](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-device-specific-registry-settings)。

### 1.2 解法（本 Lab 固件已内置）

1. **序列号固定**：`usb_descriptors.c` 用 STM32 出厂 96 位 UID（0x1FFFF7E8）生成 24 位 hex 序列号——每台设备唯一且终身不变，Windows 记住 "COM7 = 这台设备"，换口不漂移；
2. **VID/PID 不中途变更**：固件升级绝不改 VID/PID/序列号算法；实在要改（产品换代），发布说明里必须写清"客户需重配上位机口"，并建议上位机按 VID:PID+序列号动态发现（`host/serial_tool.py` 即此策略）；
3. **Win7 时代设备**：需自签 INF 指向 usbser（`UsbSerial_Install` 模板），INF 里写死 VID/PID；Win7 已停服，新设计不做兼容承诺，仅在合同里注明；
4. 客户侧应急话术："设备管理器 → 端口 → 右键属性 → 可以看设备实例路径里的序列号确认是哪台机器"；批量部署建议直接上位机枚举而非固定 COM 号。

## 2. CH340 假芯片识别与供应链

### 2.1 为什么 CDC 设备厂商要关心 CH340 假货

用 CH340/FTDI 这类"USB 转真 UART"芯片外挂 MCU，是很多过渡产品/维修件的实现方式（本 Lab 的方案是 MCU 自带 USB，不需要它们——但你的供应链里到处是它们：调试板、下载器、传感器模块）。假 CH340 的公开资料共识：

- **假货多为无丝印/磨标芯片**，功能上常是"阉割兼容"：基本收发可用，但波特率上限、握手信号、部分寄存器行为与真品有差；
- **2023 年起 WCH 官方驱动（3.8.2023.2 及以后）故意不再支持假芯片**——大量"插上不识别/驱动报错"的返修从这时爆发，微软问答上案例：[CH340 not working after Windows update](https://learn.microsoft.com/en-us/answers/questions/3937558/usb-serial-ch340-chipset-not-working-after-windows)；
- 装回旧版 3.5.2019.1 驱动可让假芯片继续工作（这正是风险：**客户电脑更新驱动 = 你的现场设备"被升级死亡"**）；
- 国内已有抄袭 CH340 的厂商被判刑的公开报道（社区讨论：[whycan: CH340 侵权案](https://whycan.com/t_7588.html)）。

识别与自救工具（社区开源）：[FakeCH340DriverFixer](https://github.com/SHWotever/FakeCH340DriverFixer)（枚举串口并给出真/假概率评估、可回装旧驱动）；识别特征汇总见 [SimHub Wiki: Counterfeit CH340G](https://github.com/SHWotever/SimHub/wiki/Arduino---Counterfeit-Fake-CH340G-chips-driver-issues)。WCH 官方产品与渠道：[wch.cn/products/ch340.html](https://www.wch.cn/products/ch340.html)。

### 2.2 供应链动作清单

1. **只从授权渠道采购**（立创商城自营、WCH 直销代理），来料抽检做"驱动 3.8+ 枚举测试"——假芯片当场现形；
2. 比行情价低得离谱的"CH340"（市场价约 ¥0.5~1.5 量级，随行情）按假货预设处理；
3. 随机板/下载器这类要出厂的附件，能用 MCU 自带 USB（CDC）就别挂转换芯片——少一颗料少一条供应链风险，这也是本 Lab 方案的隐藏优势；
4. BOM 里唯一来源料（FTDI 等）做好第二货源（WCH/沁恒兼容系列）验证，替代料同样走授权渠道。

## 3. 虚拟串口"波特率无关"的正确理解与客户沟通话术

### 3.1 正确理解

SET_LINE_CODING（bRequest=0x20）里那个 "115200" 是**抽象参数**：设备应当记住并可回读（GET_LINE_CODING，bRequest=0x21），但它**对 USB 传输速率零影响**——数据永远走全速/高速批量端点跑（FS 理论 1.5MB/s，实际 300~700KB/s 量级，见知识库 [虚拟串口ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md)）。波特率只在这些场合才有真实语义：

- 设备固件**主动读它**做行为切换（如经典 "1200bps touch"：检测到 1200bps 请求就复位进 bootloader）；
- CDC 数据落设备后**再经真 UART 转发**（网关转发场景）——那段的速率才是真的。

### 3.2 客户沟通话术（可直接抄）

- 客户说"我把波特率调到 921600 就快了？"→ "USB 这一段的速率和波特率设置无关，永远是全速 USB 在跑；您觉得快/慢的部分，瓶颈在设备端传感器采样或上位机处理。设置值只需保证设备接受即可。"
- 客户说"我老系统的协议写死了 9600"→ "没问题，参数照填，USB 会原样工作；老系统的超时参数建议按实测微调，USB 延迟特性与真串口不同（毫秒级批量调度 vs 连续字节流）。"
- **要诚实提醒的差异**：虚拟串口没有真 UART 的"逐字节到达"特性，数据按批量包（64B 块）到达，做逐字节超时协议的老上位机需要适配；两端流控（RTS/CTS）是抽象的，不产生硬件握手效果。

## 4. DFU 升级失败的回滚设计（双分区 / A-B）

### 4.1 失败模式清单（按现场频率排序）

1. **升级中断电/拔线**：最常见。DFU 逐块"先擦后写"，App 区半成品——只要 bootloader 完好，再刷一遍即可；
2. **坏固件能跑但功能错**：最难缠，需要回滚或双分区兜底；
3. **坏固件跑飞/成砖**：bootloader 的"启动计数 + 校验失败常驻 DFU"机制兜底；
4. **bootloader 自己被写坏**：设计上必须使其不可能——Flash 写保护（WRP）锁死引导区，这也是分区契约的底线。

### 4.2 架构分级（按 Flash 预算选型）

| 预算 | 架构 | 回滚能力 |
|---|---|---|
| 64KB（本 Lab F103C8T6） | 单 App + bootloader 常驻 + WRP | 无真回滚；靠"校验失败/看门狗 → 常驻 DFU 等重刷"+ BOOT0 ROM DFU 救砖 |
| 128KB~256KB | **A/B 双分区**（MCUboot swap/overwrite）：新固件写 B 区，试运行确认后才标记"确认"；失败 revert 回 A 区 | 断电/坏固件自动回滚，金标准；成本 = 一半 Flash |
| 512KB+/带外部 SPI Flash | A/B + 固件签名（MCUboot 完整形态） | 同上 + 防恶意刷入（安全合规场景） |

方案细节与选型对比见 [firmware/src/bootloader选型与跳转设计.md](firmware/src/bootloader选型与跳转设计.md)；MCUboot 的 swap 算法与 revert 策略见 [MCUboot 文档](https://docs.mcuboot.com/)。

### 4.3 现场升级规程（写给交付工程师）

1. 升级前记录当前版本（`ver` 命令/上位机读回），**失败可回滚到"至少上一个能跑的版本"**；
2. 升级期间设备断电 = 白干但不可怕；**升级完成确认前不要断开日志**（serial_tool.py 的落盘就是证据链）；
3. 首个新版本灰度：先刷 1~2 台老练 72 小时再放量；
4. 永远给客户留"最后一根线"：BOOT0 跳线 + ROM DFU 救砖流程写进产品手册（host/dfu_guide.md 第 3 节）。

## 5. 工业现场 ESD 击穿返回件分析思路

USB 口是设备的第一静电入口（人体模型 ±15kV 级、干燥冬天的现场插拔）。返回件分析（RMA FA）流程：

1. **收集现场信息**：季节湿度、插拔频次、线缆类型（屏蔽/非屏蔽）、同批返修率——ESD 返修常呈"冬季批量出现"的季节性；
2. **先复测再拆解**：直接上 SWD/USB 复测，避免拆解引入二次损伤；记录初始故障现象；
3. **定位损伤层级**（从外向内）：
   - **防护器件**：USBLC6 类是否焊装？丝印与来料真伪（假 TVS 没有钳位能力）？有无击穿短路痕迹；
   - **USB 数据脚**：万用表二极管档量 PA11/PA12 对 VDD/GND 的体二极管——ESD 打穿 MCU 内部收发器的典型特征是**对 VDD 二极管短路/漏电**，芯片微热；
   - **连接器与地**：连接器外壳地是否虚接？ESD 没有低阻回流路径时会找最近的敏感网络泄放；
4. **区分 ESD 与浪涌**：TVS 有烧痕/炸裂偏浪涌（能量大）；无痕迹但 MCU 收发器死偏 ESD（瞬间击穿）；
5. **闭环改进**（对照 hardware/设计要点.md）：防护器件靠近连接器、外壳地处理、USB 线缆加磁环、结构开孔防手指直接触碰端口；改版后按 IEC 61000-4-2 等级做整改验证（具体等级以产品标准为准）。

给硬件同事的量化解法：任何"莫名 USB 挂死，复位才好"的返修，先查防护链再怀疑固件——公开资料共识是 ESD 损伤常表现为"降级可用"，最终在某次插拔后彻底失效。

## 参考资源

- [Microsoft：支持的 USB 类（usbser.sys / CDC-ACM）](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes) · [USB 设备注册表设置](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-device-specific-registry-settings)
- [FakeCH340DriverFixer（假 CH340 检测/驱动回装工具）](https://github.com/SHWotever/FakeCH340DriverFixer) · [SimHub Wiki：假 CH340G 识别](https://github.com/SHWotever/SimHub/wiki/Arduino---Counterfeit-Fake-CH340G-chips-driver-issues) · [微软问答：Windows 更新后 CH340 失效案例](https://learn.microsoft.com/en-us/answers/questions/3937558/usb-serial-ch340-chipset-not-working-after-windows) · [WCH 官网](https://www.wch.cn/products/ch340.html)
- [MCUboot 文档（A/B 与 revert）](https://docs.mcuboot.com/) · [dfu-util](https://dfu-util.sourceforge.net/)
- 知识库：[虚拟串口 ACM](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/CDC-通信设备类/01-虚拟串口ACM.md) · [DFU 固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
- [usb.org 获取 VID](https://www.usb.org/getting-vendor-id) · [pid.codes 社区 PID](https://pid.codes/)

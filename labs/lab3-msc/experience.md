# Lab3 实战经验：MSC 落地的六个坑

> 定位：这一页是**社区经验（公开资料共识）+ 可验证工具/规范出处**的沉淀，不保证逐条有规范背书；
> 凡引用公开资料处给出链接，价格均为 2024–2026 行情区间、以实时渠道为准。
> 协议字节级内容请以知识库为准：[MSC 概述与 BOT](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md)、[SCSI 命令与 UFI](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/01-SCSI命令与UFI.md)、[Sense 全表](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/02-SCSI操作码与Sense全表.md)。

---

## 1. FAT 表掉电损坏与 sync 策略（FAT corruption on power loss）

**机制**：FAT12/16/32 在磁盘上有**两份文件分配表（FAT1/FAT2）**，外加目录项与数据簇。一次"改名/追加写"
实际要动：目录项、FAT 链、数据簇三处。写到一半掉电，三处不一致 → 出现交叉链（cross-linked clusters）、
`FOUND.000` 下的 `*.CHK` 碎片文件、甚至整卷要求格式化。TF 卡内部 FTL 的页写非原子性会放大损伤
（公开资料共识，如 Microsoft 对 FAT32 的描述与 Linux vfat 文档）。

**固件侧 sync 策略（本实验已实现的部分）**：

| 策略 | 实现点 | 效果 |
|---|---|---|
| 写缓存窗口化 | msc_app.c 的 4KB 写窗 | 换窗即 flush，掉电窗口 ≤4KB |
| SYNCHRONIZE CACHE(0x35) | `tud_msc_scsi_cb` 收到即 flush | 主机"安全弹出"时把易失缓存落盘 |
| START STOP UNIT 收尾 | `tud_msc_start_stop_cb(start=false)` → flush | 弹出的最后一拍再兜底一次 |

**产品侧策略**（记录仪/数据采集类，公开工程惯例）：

- 视频分段写（dashcam 循环录像每 1–5 分钟一个文件），段间 `fsync`/close——把最坏损失限制在一个段；
- 关键事件（碰撞信号）触发立刻 close + sync，而不是等缓冲满；
- 嵌入式端如果自己挂 FatFS，开启 `FF_FS_TINY` 无关掉电，真正相关的是**关闭写缓冲（`FF_MIN_SS` 对齐写）
  或自管 flush 时机**；对掉电更敏感的场景直接上 **journal 类文件系统（littlefs/ext4）**——littlefs 就是为
  掉电一致性设计的（https://github.com/littlefs-project/littlefs）；
- 给卡 VDD 加掉电检测（PVD/POR 中断 → 抢刷缓存）只能缓解不能根治，根治靠文件系统原子性。

**经验法则**：凡是"拔了就坏"的差评，九成不是 FAT 的锅，是**固件谎报了写入完成**（缓存没落盘就回 CSW 成功）。
本实验教学后端的 flush 时机设计就是为讲清这件事。

## 2. "安全弹出"（Safe Eject / Safely Remove）的协议含义

Windows/macOS 弹 U 盘时，主机驱动按 BOT 顺序发三件事（流程为主机驱动公开行为共识，命令语义见知识库）：

1. `PREVENT/ALLOW MEDIUM REMOVAL (0x1E)`：锁/解锁介质（本实验 `msc_app.c` 直接应答 0）；
2. `SYNCHRONIZE CACHE (0x35)`：**数据承诺点**——此后主机认为所有数据已到介质，掉电不再负责；
3. `START STOP UNIT (0x1B, START=0, LOEJ=1)`：通知设备"停转/可退卡"，固件在此把写缓存刷净、
   （产品化）给卡座断电，然后回 CSW。资源管理器消失盘符 = 这条命令成功。

所以"安全弹出"的物理含义是**主机把写缓存责任移交给介质**，而不是 mystical 停电仪式。
反推两条工程结论：

- 固件必须在 0x35/0x1B 里把缓存真正 flush，且 flush 失败要回 `MEDIUM ERROR`(3h) 而不是 0 成功；
- 直接拔盘不丢"文件内容"但可能丢"FAT 表更新"的原因：Windows 默认对可移动盘用"快速删除"策略
  （见第 4 节），日常小拷贝早已直写——但大拷贝期间拔盘必坏。

Linux 对应 `udisksctl unmount` + `power-off`（见 host/msc_linux.md），Android 手机"卸载 SD 卡"同源。

## 3. 假容量卡黑产与 f3 鉴伪（fake capacity cards / f3）

**黑产手法**（公开报道与 f3 项目文档共识）：回收/白牌小容量 NAND（如 8GB）刷控制器固件，把
`READ CAPACITY`/CSD 改成 256GB；超出真容量的写入被**静默丢弃或回绕覆盖**。买家传照片→几个月后
"照片全损坏"→数据无价，这就是黑产的利润来源。识别口诀：**价格低于同期正规卡行情一半以上的
"大容量"卡，默认假**（经验口径）；正规渠道价格区间参考：32GB A1 ¥20–35、128GB ¥60–100、
256GB ¥120–200（2024–2026 行情）。

**鉴定工具 f3（Fight Flash Fraud）**，三步走（完整命令见 [host/msc_linux.md](host/msc_linux.md#4-f3fight-flash-fraud假容量卡照妖镜)）：

```bash
f3write MOUNT/ && f3read MOUNT/    # 慢而全：全盘写入读回比对（几十 GB 要数小时）
sudo f3probe --destructive /dev/sdX # 快而狠：秒级探测真实几何（毁数据，空盘再跑）
sudo f3fix /dev/sdX                 # 按真实容量修正分区表（救盘不救数据）
```

**产品工程化**：读卡器/记录仪厂商来料抽检用 `f3probe` 批量过一遍是公开行业惯例；本实验的读卡盘
固件本身也可以当"白盒读卡器"，配合 `f3probe` 做来料检验——这是 Lab3 的真实商用姿势之一。
工具仓库：https://github.com/Digint/f3 。

## 4. Windows 写缓存策略的影响（Write-cache policy）

设备管理器 → U 盘属性 → 策略页有两个选项（Windows 行为，公开资料共识）：

| 策略 | 行为 | 代价 |
|---|---|---|
| **快速删除（Quick removal）** | 禁用 Windows 写缓存，基本直写；**可移动盘默认** | 大量小文件传输慢 |
| **更好的性能（Better performance）** | 启用写缓存 + 必须安全弹出 | 拔盘窗口变大，数据风险高 |

给固件的三个影响：

1. 默认策略下 Windows 大量小写入是**直写的**，所以固件回 CSW 成功前必须真落盘（教学后端 4KB 窗口
   flush 语义与之匹配）；用户开了"更好的性能"后，风险移交给"安全弹出"协议（第 2 节）；
2. `MODE SENSE/SELECT` 的 caching mode page 里 WCE 位是 SCSI 层的正统表达；TinyUSB 内建只支持
   MODE SENSE(6) 基础页，多数 U 盘控制器（慧荣/群联等公版）也不精细实现——主机实际上靠策略页+eject
   流程兜底（社区逆向/公开资料共识）；
3. macOS 没有这个策略页，`sync`+弹出即可；Linux 的 `mount -o sync` 等价于强制直写（性能换安全）。

## 5. UAS 优先导致的兼容问题（UAS vs BOT quirks）

**现象**：同一只 USB-SATA 桥/读卡器，在某些 Linux 机器上疯狂掉盘（`usb disconnect` 日志），
Windows 却正常。根因（内核社区公开共识）：现代内核对声明支持 UASP（USB Attached SCSI Protocol，
BOT 的继任者，走流 Stream + 中断端点）的设备**优先绑定 `uas` 驱动**，而不少廉价桥接芯片的 UAS 实现
有缺陷（消息配对、流管理 bug），触发后设备离线。识别方法：`dmesg` 里是 `uas` 还是 `usb-storage`。

**规避/修复**（公开资料，Arch Wiki 有完整条目）：

```bash
# 永久把该设备打回 BOT（usb-storage）：VID:PID 从 lsusb 拿
echo "options usb-storage quirks=0bc2:ab20:u" | sudo tee /etc/modprobe.d/uas-blacklist.conf
```

注意方向性：**我们自己用 TinyUSB 枚举成 BOT 设备（0x50 协议）时，主机根本不会尝试 UAS**——
所以本实验固件不受影响；受影响的是"用现成桥接芯片/读卡器做外设"的产品选型。
选型口诀：外壳/桥接方案上线前，在 Linux 跑 24h `f3write/f3read` 压测并确认驱动名。
参考：https://wiki.archlinux.org/title/UAS ；内核 quirks 文档见
https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html（`usb-storage.quirks`）。

## 6. NAND 寿命、磨损均衡与"直用 TF 卡"的保修现实

**寿命模型**（公开行业常识）：NAND 按 Program/Erase（P/E）次数计寿命：SLC ~10 万、MLC ~3 千–1 万、
TLC ~500–3000、QLC 数百。TF 卡内部控制器自带**磨损均衡（wear leveling）+ 坏块管理**，但它是黑盒：
固件无法获知卡的真实磨损，`SD Status` 里的寿命指示位（`Life Time Estimation`）只有新卡才可靠。

**记录仪场景的数学**：128GB 卡 × 3000 P/E（TLC 理论）≈ 384TB 写入寿命（理论乐观值）；记录仪双通道
720p 每天写 20–30GB，两三年就逼近公开评测里"卡变只读/掉速"的高发区间——所以产品级记录仪都推荐
**High Endurance（高耐久）卡**，或干脆内置 eMMC/pSLC 自管 FTL：

| 方案 | 容量 | 寿命口径 | 价格区间（参考行情） |
|---|---|---|---|
| 消费 TF（TLC） | 32–256GB | 循环录像 1–3 年 | 20–200 元 |
| 高耐久 TF（Dashcam/监控标注） | 64–256GB | 厂商标称数千小时连续写 | 60–250 元 |
| 工业 pSLC TF / 工业卡 | 8–32GB | 10 倍级 P/E，宽温 | 100–400 元 |
| 板载 eMMC + 自管 FTL | 8–32GB | 寿命可控可上报 | 芯片 15–60 元 |

**保修/返修现实**（渠道公开条款的常识性总结，具体以厂商条款为准）：消费 TF 卡的保修几乎都**不含数据
恢复**，只换卡不救数据；"卡坏了"的举证责任在用户（要开卡检测报告）；因此做"记录仪导出"这类数据价值
高的产品时，商业上真正的问题不是卡寿命，而是**丢数据的责任边界**——产品文档要写清"卡是耗材"，
并优先考虑第 1 节的 sync/断电保护设计来压缩返修争议。

**给本实验的映射**：`msc_app.c` 注释里"写放大 8 倍"的教学后端，就是把这类现实问题搬进代码的最小样本——
真实的 U 盘控制器，贵就贵在这层 FTL 算法上（Flash 友好的合并写、冷热数据分离、掉电恢复表）。

---

## 一页速查

| 症状 | 第一嫌疑 | 验证工具 |
|---|---|---|
| 拷完文件拔盘后 FAT 损坏 | 固件谎报写入完成 / 没等 0x35 | usbmon 抓 SYNCHRONIZE CACHE；`fsck.vfat -n` |
| 插卡系统重启 | 卡插入浪涌拉垮 3.3V | 示波器看 VDD（hardware/设计要点.md 第 3 节） |
| 卡标称 256G 实际只能存 30G | 假容量卡 | `f3probe`（host/msc_linux.md 第 4 节） |
| Linux 上读卡器反复掉盘 | 桥接芯片 UAS 缺陷 | `dmesg` 看 uas；`usb-storage.quirks` 打回 BOT |
| 用一年后卡变只读 | NAND 磨损到头（TLC 记录仪常态） | SD Status Life Time；换高耐久卡 |

## 参考资源

- f3（Fight Flash Fraud）— https://github.com/Digint/f3
- littlefs（掉电一致嵌入式文件系统）— https://github.com/littlefs-project/littlefs
- Arch Wiki: UAS — https://wiki.archlinux.org/title/UAS ；内核 `usb-storage.quirks` — https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
- Microsoft FAT32 结构文档（文件系统层权威描述）— https://learn.microsoft.com/windows/win32/fileio/file-systems
- JEDEC JESD84（eMMC）与 SD Association 规范页 — https://www.jedec.org/ · https://www.sdcard.org/downloads/pls/
- USBTree 知识库 MSC 分枝 — https://github.com/tangjianfang/USBTree/tree/main/20-枝干-设备类协议/MSC-大容量存储

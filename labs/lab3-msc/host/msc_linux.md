# Lab3 主机侧：Linux 挂载、验证与 f3 鉴伪

> 先纠正一个新手直觉：**MSC 设备没有串口**。CDC 串口是 Lab2 的角色；MSC 在 Linux 里是一个
> **块设备（block device，`/dev/sdX`）**。想看"设备说了什么"用抓包（[capture/BOT读扇区帧分析.md](../capture/BOT读扇区帧分析.md)），
> 想看固件日志用 UART——主机侧的观测武器是 `dmesg` / `lsblk` / `f3`。

## 1. 插上设备：内核视角

```bash
$ sudo dmesg -w        # 插入设备后观察
usb 1-1.2: new full-speed USB device number 5 using xhci_hcd
usb-storage 1-1.2:1.0: USB Mass Storage device detected      # ← 驱动认出 08h/06h/50h
scsi host6: usb-storage 1-1.2:1.0
sd 6:0:0:0: [sda] 1024 512-byte logical blocks: (524 kB / 512 KiB)   # ← READ CAPACITY 的结果
sd 6:0:0:0: [sda] Attached SCSI removable disk
```

逐行对照固件：第 1 行是枚举（描述符阶段）；第 2 行是接口类签名命中 `usb-storage`；
第 3 行是 `READ CAPACITY(10)`——你固件里 `tud_msc_capacity_cb` 报的 `block_count` 直接出现在
`1024 512-byte logical blocks` 里。**块数对不上 100% 是容量上报的锅**，从这里开始定位。

> 看到 `uas` 而不是 `usb-storage`？你的设备被主机升级到了 UAS 协议（BOT 的继任者）。
> TinyUSB 目前枚举成 BOT 设备不会触发；用现成读卡器做实验会遇到，详见 [experience.md](../experience.md) 第 5 节。

## 2. 找到它、挂载它

```bash
$ lsblk -o NAME,SIZE,TYPE,TRAN,VENDOR,MODEL,MOUNTPOINTS
NAME   SIZE TYPE TRAN VENDOR   MODEL      MOUNTPOINTS
sda    512K disk usb  USBLabs  MSC Disk
└─sda1                       # 没有分区/未格式化 → 主机会提示格式化

# 方式 A：桌面环境自动挂到 /media/$USER/xxx（udisks2 在干活）

# 方式 B：手工（脚本里常用）
$ sudo mkfs.vfat -F16 /dev/sda        # 512KB RAM 盘用 FAT16；≥64MB 可 -F32
$ udisksctl mount -b /dev/sda1
Mounted /dev/sda1 at /media/tjf/3633-19AF.

# 考机：全盘读写一遍
$ dd if=/dev/urandom of=/media/tjf/3633-19AF/test.bin bs=64K count=8 status=progress
$ sync                                # ← 关键：dd 返回 ≠ 数据落卡，见下节
$ sha1sum /media/tjf/3633-19AF/test.bin   # 拔下来再读回对比
```

### `sync` 与"安全弹出"在 Linux 里的样子

```bash
$ udisksctl unmount -b /dev/sda1      # 卸载文件系统（刷盘）
$ udisksctl power-off -b /dev/sda     # 总线层面停用（触发 SYNCHRONIZE CACHE + START STOP UNIT）
```

这条链路对应的协议字节（`SYNCHRONIZE CACHE(10)`→`START STOP UNIT`）与"为什么 dd 完直接拔不丢文件但会丢 FAT 表"，
见 [experience.md](../experience.md) 第 1、2 节。

## 3. 小盘格式化的坑（RAM 盘专用）

Windows 对小于 ~33 MB 的卷不提供 FAT32（其实现要求 FAT32 最小 65527 簇），格式化对话框里根本不出现该选项；
512 KB–4 MB 的 RAM 盘请在 Linux 格式化为 FAT12/16（`mkfs.vfat` 自动选或 `-F16` 指定），或把固件
`RAMDISK_BLOCK_COUNT` 调大。这是教学盘特有的坑，TF 卡真盘不会遇到。

## 4. f3：Fight Flash Fraud，假容量卡照妖镜

> f3 是开源的假卡鉴定工具（作者 Digiratii/石刻的 f3 项目）——**并入本实验的 host/f3_guide**。
> 背景：市面黑产把小容量卡刷固件伪装成大容量（见 experience.md 第 3 节），
> 写超出真容量的数据会静默丢失/循环覆盖。任何做记录仪/读卡器产品的人都该会这套工具。

### 4.1 安装

```bash
sudo apt install f3          # Debian/Ubuntu
# brew install f3            # macOS
# Windows：用 WSL，或 F3X（第三方 GUI 封装）
```

### 4.2 例行验证（f3write/f3read，非破坏但要求全盘可用）

```bash
$ f3write /media/tjf/MOUNT/ && f3read /media/tjf/MOUNT/
...
Valid sector range: (4, 100%)
Average writing speed: 18.20 MB/s
Average reading speed: 32.11 MB/s
```

原理：写入端用固定模式填满全盘并记录校验，读出端逐文件比对。
**盘小时快、盘大时要几小时**——这是它唯一的缺点。

### 4.3 快速鉴定（f3probe，秒级，破坏性）

```bash
$ sudo f3probe --destructive --time-ops /dev/sda
WARNING: Probing is destructive...    # 全盘数据会被毁，盘上没东西再跑
Good news: Device /dev/sda is real
    Device geometry:
        * Real size: 61.91 GB (130054144 512-byte blocks)
        * Announced size: 256 GB (500107862 512-byte blocks)   # ← 标称 256G，实际 62G：假卡实锤
```

关键输出解读：
- `Device is real`：真容量 ≥ 标称（可靠盘）；
- `Device is counterfeit`：假卡，接着给出真实几何与假容量映射方式；
- `--time-ops` 顺带报真实读/写/擦除单次耗时（选卡做记录仪时的原始性能数据）。

### 4.4 假卡修复（仅数据抢救意义）

```bash
$ sudo f3fix /dev/sda      # 按探测出的真实容量重写分区表，把假容量部分排除在外
```

f3fix 能让假卡"按真实容量继续用"，但黑卡固件本身不可信（掉电/换卡行为无保证），**不要用于任何正经数据**。

### 4.5 实验闭环

把一张来路不明的卡插到本实验的读卡盘上 → `f3probe` 鉴定 → 换正规卡对比 →
把两份报告写进你的实验记录。这就是"读卡器量产前用 f3 抽检来料"的流程原型（社区/厂商公开经验共识）。

## 5. 性能基准速查

```bash
# 顺序写（注意 oflag=direct 绕过页缓存，否则测的是内存）
$ dd if=/dev/zero of=/media/tjf/MOUNT/t.bin bs=1M count=64 oflag=direct status=progress
# 顺序读
$ dd if=/media/tjf/MOUNT/t.bin of=/dev/null bs=1M iflag=direct status=progress
```

预期量级（公开评测共识）：RP2040+SPI 全速版 ~0.6–0.9 MB/s（FS USB 12Mbps 上限 ~1.1 MB/s 是天花板）；
CH32V307 HS + SDIO4bit 可到 20 MB/s 级。写速度显著低于读速度是 TF 卡正常现象（FTL 后台整理）。

## 参考资源

- f3（Fight Flash Fraud）官方仓库 — https://github.com/Digint/f3
- udisks2 文档 — https://www.freedesktop.org/software/udisks/doc/
- usb-storage 内核文档与 usb-storage.quirks 参数 — https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html
- Linux usbmon 协议文档 — https://www.kernel.org/doc/html/latest/driver-api/usb/usbmon.html
- USBTree：[06-四种传输类型](https://github.com/tangjianfang/USBTree/blob/main/10-树干-USB核心/06-四种传输类型.md) · [MSC 概述与 BOT](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/MSC-大容量存储/00-MSC概述与BOT.md)

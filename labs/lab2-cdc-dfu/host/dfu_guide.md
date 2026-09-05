# dfu-util 全流程指南（Lab2）

> 配套：[firmware/src/bootloader选型与跳转设计.md](../firmware/src/bootloader选型与跳转设计.md)（分区契约与方案对比）
> 概念背景：知识库 [01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)
> 工具主页：[dfu-util](https://dfu-util.sourceforge.net/)（[手册](https://dfu-util.sourceforge.net/dfu-util.1.html)），Windows/macOS 在其 Releases 下载，Linux `sudo apt install dfu-util`。

## 0. 前置：本 Lab 的两个 DFU 通道

| 通道 | 进入方式 | 枚举身份 | dfu-util 参数 |
|---|---|---|---|
| **自研/TinyUSB DFU bootloader**（12KB @ 0x08000000） | `dfu-util -e`（DFU_DETACH）或串口命令 `dfu` | 你自己 VID/PID，接口 Protocol=0x02（DFU 模式） | `-a 0 -D app.bin`（地址由 bootloader 决定） |
| **ST ROM DFU（DfuSe）**（救砖保底） | J1 跳线 BOOT0=1 + 复位（见 ../hardware/设计要点.md 第 5 节） | `0483:df11` "STM32 Device in DFU Mode" | `-a 0 -s 0x08003000:leave -D app.bin` |

## 1. 列出设备：`-l` 永远是第一步

```bash
dfu-util -l
```

典型输出解读（ROM DFU 为例）：

```
Found DFU: [0483:df11] verno=2200, devnum=12, cfg=1, intf=0, alt=0, name="@Internal Flash  /0x08000000/01*064Ke", serial="..."
Found DFU: [0483:df11] ..., alt=1, name="@Option Bytes  /0x1FFFF800/01*016 e"
```

- `@Internal Flash /0x08000000/01*064Ke`：alt 0 = 内部 Flash，起始 0x08000000，64KB；
- `alt`（Alternate Setting）是 DfuSe 的"目标"选择：写固件用 alt 0，别选到 Option Bytes；
- 没列出设备：先看 `lsusb`/设备管理器里有没有 `0483:df11`，有但 dfu-util 看不到通常是 Windows 驱动未换 WinUSB/无 libusb——用 [Zadig](https://zadig.akeo.ie/) 给 `STM32 DFU` 设备装 WinUSB。

## 2. 通道一：Runtime → DFU 模式（不拔线升级）

```bash
# ① 让运行态设备分离复位（等价于主机发 DFU_DETACH, bmRequestType=0x21 bRequest=0x00）
dfu-util -e          # --detach，作用于 Runtime 模式接口（Protocol=0x01）
# 设备会"消失"几秒，再以 DFU 模式重新枚举

# ② 等待重新枚举后下载（脚本里用循环等，见第 5 节）
dfu-util -a 0 -D lab2_app.bin

# ③ 自定义 bootloader 若实现了 DfuSe 风格地址命令，可显式给地址：
dfu-util -a 0 -s 0x08003000 -D lab2_app.bin      # 从 App 分区起点写入
#    `:leave` 后缀（DfuSe 专用）= 写完即跳转，不等待复位：
dfu-util -a 0 -s 0x08003000:leave -D lab2_app.bin
```

> DFU 1.1 与 DfuSe 的差异：`-s :leave`/`:leave` 是 ST 的 DfuSe 扩展命令编码（见 [AN3156](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)），标准 DFU 1.1 设备（含 TinyUSB dfu 例程）不认识它——纯 DFU 1.1 流程是"DNLOAD(wLength=0) 结束 → GETSTATUS 到 manifestation 完成 → 设备自行复位（bit3=1 时）或主机 `dfu-util -R` 复位"。

## 3. 通道二：ST ROM DFU 救砖（BOOT0 + DfuSe）

```bash
# J1 短接 BOOT0=3.3V → 按复位 → 设备以 0483:df11 枚举
dfu-util -l                                        # 确认 @Internal Flash 0x08000000

# 把 App 写到 0x08003000（分区契约！），写完 :leave 直接运行
dfu-util -a 0 -s 0x08003000:leave -D lab2_app.bin

# 回 SYS 内存引导：拔跳线 BOOT0=0 复位，CDC 重新出现
```

ROM DFU 的擦除是隐式的（按写入地址逐页擦），也可显式整片擦除后重写：

```bash
dfu-util -a 0 -s 0x08003000 -D lab2_app.bin -v     # -v 校验（对比读回）
```

## 4. 固件文件准备：bin + DFU 后缀

`-D` 接受裸 `.bin`（配合 `-s` 地址），也可用带 DFU 后缀的 `.dfu`（VID/PID/bcdDevice 匹配校验，规范见知识库 DFU 文档第 9 节）：

```bash
# 用 dfu-util 套装里的 dfu-suffix 给 bin 追加 16 字节后缀（含 CRC32）
dfu-suffix -v 0xCAFE -p 0x4010 -a lab2_app.bin     # -> lab2_app.dfu 语义
dfu-util -l                                        # 校验后缀与设备匹配
dfu-util -a 0 -D lab2_app.bin
```

量产流水线建议：CI 产出 `.bin` + `.dfu` 双产物，`.dfu` 内嵌 VID/PID 防止"刷到别的产品头上"。

## 5. 脚本化升级（现场批量部署形态）

bit3（manifestation tolerant）=0 的设备收尾阶段会"消失再重枚举"，脚本必须等它回来：

```bash
#!/bin/bash
set -e
dfu-util -e || true
# 轮询等待 DFU 模式设备出现（最长 30s）
for i in $(seq 1 30); do
  if dfu-util -l 2>/dev/null | grep -q 'Found DFU'; then break; fi
  sleep 1
done
dfu-util -a 0 -s 0x08003000 -D lab2_app.bin -v
dfu-util -R                      # 复位设备（-R = Reset after transfer）
# 再等 CDC 回来
for i in $(seq 1 30); do
  ls /dev/ttyACM* 2>/dev/null && break; sleep 1
done
echo "upgrade done"
```

## 6. 升级失败排查速查

| 症状 | 先查什么 |
|---|---|
| `dfu-util -e` 无反应 | Runtime 接口是否真有 DFU（`dfu-util -v -l` 看 alt 名）；描述符 Protocol 是否 0x01 |
| DFU 模式下载中途断/errWRITE | Flash 时序：bootloader 是否按 `bwPollTimeout` 等待（GETSTATUS 在 dfuDNBUSY 期可能被拒） |
| 写完不跳转 | `-s :leave` 只对 DfuSe 有效；纯 DFU 1.1 要 `-R` 或等 manifestation tolerant 行为 |
| 写到 0x08003000 报 errADDRESS | 分区表与地址不符；bootloader 的地址白名单没包含该区 |
| 设备"失踪"找不回 | BOOT0+ROM DFU 救砖（第 3 节）；bootloader 完好性是底线（WRP 写保护） |

更多工程侧的回滚/双分区讨论：[../experience.md](../experience.md) 第 4 节。

## 参考资源

- [dfu-util 主页](https://dfu-util.sourceforge.net/) · [dfu-util 手册页](https://dfu-util.sourceforge.net/dfu-util.1.html)（`-s` 命令编码细节）
- [AN3156 — STM32 bootloader 中的 USB DFU 协议（DfuSe）](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
- [AN2606 — 系统存储器启动模式](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf)
- [USB DFU 1.1 规范](https://www.usb.org/sites/default/files/DFU_1.1.pdf)
- 知识库：[01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)（状态机、错误码、GETSTATUS 6 字节格式）

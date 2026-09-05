# Bootloader 选型与跳转设计（DFU 分区契约）

> 上游文档：[../README.md](../README.md)（构建与分区表）· 知识库 [01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)（状态机/请求全表）
> 本文不要求实现完整 bootloader，给出**分区契约、跳转时序、方案对比与选型建议**，Lab 阶段的保底升级通道是 ST ROM DFU。

## 1. 分区契约（全栈一致的"宪法"）

以 STM32F103C8T6（64KB Flash）为例——**任何一层改动分区，都必须同步下面这张表、链接脚本、host/dfu_guide.md 的烧写地址**：

| 分区 | 地址范围 | 大小 | 内容 |
|---|---|---|---|
| Bootloader | `0x08000000` ~ `0x08002FFF` | 12KB | DFU 模式 USB 栈 + 状态机 + 校验 + 跳转 |
| App（本工程） | `0x08003000` ~ `0x0800FFFF` | 52KB | TinyUSB CDC + DFU Runtime 复合设备 |

App 侧必须遵守的三件事（src/main.c 已体现）：

1. 链接脚本 `FLASH ORIGIN=0x08003000, LENGTH=52K`；
2. 向量表重定位 `SCB->VTOR = 0x08003000`（或 `VECT_TAB_OFFSET 0x3000`）；
3. `.noinit` 段留 4 字节 `g_dfu_magic`（复位不清除），用于 App→BL 传递"进入 DFU 模式"意图。

12KB 是否够放"TinyUSB DFU + CRC 校验 + 跳转"？属于紧凑可行规模，**以实际编译产物为准**；超出就先精简（去掉日志/查表 CRC 换逐字节 CRC），再考虑扩为 16KB 并把 App 起点改 `0x08004000`。

## 2. 升级流程与跳转时序

### 2.1 正常升级路径（Runtime 模式 → DFU 模式 → 新固件）

```
主机 dfu-util -e（DFU_DETACH, bmRequestType=0x21, bRequest=0x00）
   │
   ▼
App: tud_dfu_runtime_reboot_to_dfu_cb()
     g_dfu_magic = "DFU1" → NVIC_SystemReset()          ← src/main.c 已实现
   │
   ▼
Bootloader 上电：
   if (g_dfu_magic == MAGIC) { 清魔法数; 常驻 DFU 模式; }   ← 复位传递意图
   else if (App 签名/校验 OK 且 启动计数未超限) { 跳转 App; }
   else { 常驻 DFU 模式（救砖兜底）; }
   │
   ▼
DFU 模式重新枚举（建议换 PID，如 STM32 ROM 的 0x0483/0xDF11）
   主机：DFU_DNLOAD(块0..n) ↔ DFU_GETSTATUS 轮询（写 Flash 慢，按 bwPollTimeout 等）
   主机：DFU_DNLOAD(wLength=0) 宣告结束 → manifestation → 复位
   │
   ▼
Bootloader 校验新 App → 跳转 → 主机看到 CDC 设备重新出现
```

### 2.2 App → Bootloader 跳转序列（自研 bootloader 的关键代码形态）

```c
typedef void (*pAppEntry)(void);

static void jump_to_app(uint32_t app_base)
{
    uint32_t app_sp   = *(volatile uint32_t *)(app_base);          /* 栈顶 */
    uint32_t app_pc   = *(volatile uint32_t *)(app_base + 4);      /* Reset_Handler */
    if ((app_sp & 0xFF000000u) != 0x20000000u) { /* SRAM 地址合理性粗检 */ stay_in_dfu(); }

    __disable_irq();
    /* 撤销 App 之前的一切外设状态：USB/时钟/引脚回到复位态 */
    RCC->APB1ENR &= ~RCC_APB1ENR_USBEN;   /* 关 USB */
    usb_dp_pull_down_softconnect();       /* D+/D- 回到 SE0，主机看到"拔出" */
    SysTick->CTRL = 0;
    for (int i = 0; i < 8; i++) NVIC->ICER[i] = 0xFFFFFFFFu;  /* 清挂起+关中断 */
    SCB->VTOR = app_base;
    __set_MSP(app_sp);
    ((pAppEntry)app_pc)();
}
```

要点：**跳转前必须把 USB 收发器拉回空闲态**（软连接断开或 D+/D- 置 SE0），否则主机端旧设备句柄没死、新设备枚举不出来，表现为"升级完成后必须拔插一次"。按 [RM0008](https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) 复位 USB 宏单元（CNTR.PDWN/FRES 序列），细节以参考设计为准。

## 3. 方案对比（怎么选）

| 方案 | 是什么 | 优点 | 缺点 | 适用 |
|---|---|---|---|---|
| **ST ROM DFU（DfuSe）** | 芯片出厂内置的系统 bootloader（AN2606），BOOT0=1 复位进入，USB 枚举 `0x0483/0xDF11` | 零代码、**永远不会被你写坏**、dfu-util 生态现成 | DfuSe 是 ST 私有扩展（[AN3156](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)）；无签名/回滚；ROM 里无 CDC——升级时应用断线 | 本 Lab 保底通道；所有 F1/F2/F3/F4/L 系列 |
| **TinyUSB 自研 DFU bootloader** | `examples/device/dfu` 移植进 12KB 分区，标准 DFU 1.1 | 协议标准（dfu-util 直连）；可加版本/CRC/厂商命令；升级时可短暂枚举 CDC 报进度 | 要自己保证"写坏自己"不可能；占掉 12~16KB | 商业产品的默认演进路线 |
| **MCUboot** | 开源安全 bootloader（[docs.mcuboot.com](https://docs.mcuboot.com/) / [GitHub](https://github.com/mcu-tools/mcuboot)），签名镜像 + swap/overwrite 升级 | 镜像**签名防篡改**、A/B 回滚、revert 策略成熟；社区活跃 | 依赖闪存搬家（swap）机制与 MCUMgr 工具链；对 64KB 小芯片偏重，适合 128KB+ | 有安全合规要求（支付/车联网/医疗）的量产 |
| **ST Open Bootloader（X-CUBE-SBSFU 内）** | ST 官方开源可裁剪 bootloader，带安全启动+固件解密 | 官方维护、文档全、离 DFU 类协议可自定义 | 移植工作量大于 TinyUSB DFU；体积 32KB+ | 中大型 STM32 项目 |
| （旁支）UF2/MSC | RP2040 BOOTSEL：拖拽 .uf2 | 用户体验最简单 | 非 DFU 协议，dfu-util 不适用 | RP2040 生态 |

**本 Lab 建议**：量产前评估 = ROM DFU（救砖）+ TinyUSB DFU（日常升级，12KB 分区）双通道；产品有安全要求再上 MCUboot。参考链接：TinyUSB [examples/device/dfu](https://github.com/hathach/tinyusb/tree/master/examples/device/dfu) 与 [dfu_runtime](https://github.com/hathach/tinyusb/tree/master/examples/device/dfu_runtime)。

## 4. 失败与回滚设计要点（详见 ../experience.md 第 4 节）

- **写坏的可能窗口只有 bootloader 本身**：用 Flash 写保护（WRP）锁死 12KB 引导区，RDP 按返修策略权衡；
- 64KB 芯片放不下 A/B 双分区——回滚退化为"App 校验失败 → 常驻 DFU 模式等待重刷"的**最小可救砖形态**；真 A/B/双回滚需要 128KB+ 或外置 SPI Flash（MCUboot 的 swap 思路）；
- 每次上电在 RAM 记"启动计数"，App 跑通后再写"启动成功"标志：连续 N 次未确认即视作坏固件，拒绝跳转；
- 升级断电安全：DFU_DNLOAD 是"先擦后写、逐块生效"，断电最坏结果是 App 区不完整——只要 bootloader 完好就能再刷一遍，这是分区契约要守住的底线。

## 参考资源

- [USB DFU 1.1 规范](https://www.usb.org/sites/default/files/DFU_1.1.pdf)（状态机与错误码的权威定义）
- [AN3156 — STM32 bootloader 的 USB DFU 协议（DfuSe 命令集）](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
- [AN2606 — STM32 系统存储器启动模式（ROM bootloader 支持表）](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf)
- [MCUboot 文档](https://docs.mcuboot.com/) · [MCUboot GitHub](https://github.com/mcu-tools/mcuboot)
- [X-CUBE-SBSFU（含 ST Open Bootloader）](https://www.st.com/en/embedded-software/x-cube-sbsfu.html)
- [TinyUSB dfu / dfu_runtime 例程](https://github.com/hathach/tinyusb/tree/master/examples/device/dfu) · [dfu-util](https://dfu-util.sourceforge.net/)
- 知识库：[01-DFU固件升级](https://github.com/tangjianfang/USBTree/blob/main/20-枝干-设备类协议/其他设备类/01-DFU固件升级.md)

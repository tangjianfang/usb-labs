/* Lab3 MSC —— TinyUSB 大容量存储应用层（BOT 对接点 + 块设备层）
 *
 * ================= 这一层在协议栈里的位置（对照 USBTree《MSC 概述与 BOT》） =================
 *
 *   主机 PC                                    本固件
 *  ┌─────────────────────┐                 ┌──────────────────────────────┐
 *  │ FAT/NTFS 文件系统    │                 │ 本文件 msc_app.c             │ ← SCIS 命令落点
 *  │ usb-storage/uas 驱动 │  <== BOT ==>   │ TinyUSB msc_device.c（栈）    │ ← CBW/CSW 解析
 *  │ USB 控制器           │                 │ RP2040 USB 控制器            │
 *  └─────────────────────┘                 └──────────────────────────────┘
 *
 * BOT（Bulk-Only Transport）把"一条 SCSI 命令"变成批量端点上的三段：
 *   ① CBW（31B，Bulk OUT）—— 主机说"执行 CDB，数据方向/长度如下"
 *   ② 数据（方向由 bmCBWFlags 决定，长度 = dCBWDataTransferLength）
 *   ③ CSW（13B，Bulk IN）  —— 设备回 bCSWStatus：0x00 成功 / 0x01 失败 / 0x02 阶段错误
 *
 * TinyUSB 已经把 CBW/CSW 的封包、tag 配对、STALL/复位恢复全部做完；
 * **固件只按命令类型被回调**。真实帧字节长什么样见 capture/BOT读扇区帧分析.md。
 *
 * 主机挂盘必经的命令与回调对应关系（≈ U 盘最小可行命令集 8 条中的 6 条）：
 *   INQUIRY(0x12)              → tud_msc_inquiry_cb()        厂商/产品/版本 36B
 *   TEST UNIT READY(0x00)      → tud_msc_test_unit_ready_cb() 卡在不在？ false 时主机查 Sense
 *   REQUEST SENSE(0x03)        → 栈内建，数据来自 tud_msc_set_sense() 设置的值
 *   READ CAPACITY(10)(0x25)    → tud_msc_capacity_cb()       块数/块长（容量上报！）
 *   READ(10)(0x28)             → tud_msc_read10_cb()         真正搬数据
 *   WRITE(10)(0x2A)            → tud_msc_write10_cb()        真正搬数据
 *   其余（SYNCHRONIZE CACHE、PREVENT/ALLOW 等）→ tud_msc_scsi_cb()
 *   START STOP UNIT(0x1B)      → tud_msc_start_stop_cb()     安全弹出的收尾命令
 *   PREVENT/ALLOW REMOVAL(0x1E)→ tud_msc_scsi_cb()（新栈有独立弱回调，见注释）
 */

#include <string.h>
#include "tusb.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "sd_spi.h"

/*=========================== 块设备参数 ===========================*/
#define DISK_BLOCK_SIZE      512u

/* IO 活动指示（main.c 的读写灯读这个计数器） */
volatile uint32_t msc_io_ticks = 0;

/* 块级暂存缓冲：TinyUSB 回调全部在 tud_task() 单任务上下文串行执行，静态缓冲安全 */
static uint8_t s_stage[DISK_BLOCK_SIZE];

/*=========================== 后端 A：Flash RAM 盘（默认） ===========================*/
/* 把 RP2040 片外 Flash 的尾部划一块当"盘"：
 *   - 读 = XIP 直接寻址，零成本；
 *   - 写 = 先进 4KB RAM 窗口缓存，换窗/SYNC/停盘时擦写落盘（Flash 擦写粒度 4KB）。
 * 这是"写缓存（write-back cache）+ 读直通（read-through）"的最小教学实现，
 * 也是掉电丢数据风险的真实来源 —— 见 experience.md 的 sync 策略讨论。 */
#ifndef MSC_USE_SD_CARD

#ifndef RAMDISK_BLOCK_COUNT
#define RAMDISK_BLOCK_COUNT   1024u      /* 512KB。4MB Flash 板可改 4096（2MB） */
#endif
#define RAMDISK_BYTES         (RAMDISK_BLOCK_COUNT * DISK_BLOCK_SIZE)
#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES 未定义：请用 pico-sdk 标准目标编译（PICO_BOARD 已设置）"
#endif
#define RAMDISK_BASE_OFFSET   (PICO_FLASH_SIZE_BYTES - RAMDISK_BYTES)  /* 盘区放 Flash 尾部 */

#define FLASH_SECTOR_SIZE     4096u
#define SECTORS_PER_FLASH_SEC (FLASH_SECTOR_SIZE / DISK_BLOCK_SIZE)
#define CACHE_LBA_INVALID     0xFFFFFFFFu

/* 4KB 写缓存窗口 */
static struct {
    uint32_t base_lba;                  /* 窗口首扇区号 */
    bool     loaded;
    bool     dirty;
    uint8_t  data[FLASH_SECTOR_SIZE];
} s_cache;

static inline const uint8_t *flash_ptr(uint32_t lba) {
    return (const uint8_t *) (XIP_BASE + RAMDISK_BASE_OFFSET + (uint32_t) (lba * DISK_BLOCK_SIZE));
}

/* 把脏窗口擦写回 Flash。期间必须关中断：XIP 总线被 flash 编程独占，
 * 此时若 USB 中断再从 Flash 取指会锁死（pico-sdk flash API 的硬性要求）。 */
static void ramdisk_flush(void) {
    if (!s_cache.loaded || !s_cache.dirty) return;
    uint32_t addr = RAMDISK_BASE_OFFSET + s_cache.base_lba * DISK_BLOCK_SIZE;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
    flash_range_program(addr, s_cache.data, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    s_cache.dirty = false;
}

/* 保证 lba 所在 4KB 窗口已在缓存（写前装填，读自己刚写的值） */
static void ramdisk_cache_window(uint32_t lba) {
    uint32_t base = lba - (lba % SECTORS_PER_FLASH_SEC);
    if (s_cache.loaded && s_cache.base_lba == base) return;
    ramdisk_flush();                                   /* 换窗前先落盘 */
    memcpy(s_cache.data, flash_ptr(base), FLASH_SECTOR_SIZE);
    s_cache.base_lba = base;
    s_cache.loaded   = true;
    s_cache.dirty    = false;
}

static uint32_t ramdisk_block_count(void) { return RAMDISK_BLOCK_COUNT; }
static bool     ramdisk_ready(void)       { return true; }
static bool     ramdisk_writable(void)    {
#ifdef MSC_EXPORT_READONLY
    return false;   /* 记录仪导出模式：只读枚举，PC 拷得出写不进 */
#else
    return true;
#endif
}

static void ramdisk_read(uint32_t lba, uint8_t *buf, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++, lba++) {
        if (s_cache.loaded && lba >= s_cache.base_lba &&
            lba < s_cache.base_lba + SECTORS_PER_FLASH_SEC) {
            memcpy(buf, s_cache.data + (lba - s_cache.base_lba) * DISK_BLOCK_SIZE, DISK_BLOCK_SIZE);
        } else {
            memcpy(buf, flash_ptr(lba), DISK_BLOCK_SIZE);   /* XIP 读直通 */
        }
        buf += DISK_BLOCK_SIZE;
        msc_io_ticks++;
    }
}

static void ramdisk_write(uint32_t lba, const uint8_t *buf, uint32_t nblk) {
    for (uint32_t i = 0; i < nblk; i++, lba++) {
        ramdisk_cache_window(lba);
        memcpy(s_cache.data + (lba - s_cache.base_lba) * DISK_BLOCK_SIZE, buf, DISK_BLOCK_SIZE);
        s_cache.dirty = true;
        buf += DISK_BLOCK_SIZE;
        msc_io_ticks++;
        /* 教学缓存只留 4KB 窗：顺序大文件写会频繁换窗（每 4KB 一次擦写，~ms 级）。
         * 量产优化方向：多窗口 LRU / 4KB 对齐合并写 / 核 1 后台刷盘（RP2040 双核）。
         * 注意：写放大（512B 改动 → 4KB 擦写）在此后端被放大 8 倍，Flash 寿命敏感
         * 的产品必须做合并 —— 真实 U 盘控制器的核心价值就在这层算法。 */
    }
}

static void ramdisk_init_backend(void) {
    s_cache.base_lba = CACHE_LBA_INVALID;
    s_cache.loaded   = false;
    s_cache.dirty    = false;
    /* Flash 出厂态全 0xFF → 首次使用主机提示"未格式化"，属预期行为 */
}

/*=========================== 后端 B：SPI TF 卡（真盘） ===========================*/
#else

static uint32_t sd_backend_block_count(void) { return sd_spi_sector_count(); }
static bool     sd_backend_ready(void) {
    /* 每次命令前确认驱动仍可用（拔卡后 sd_spi_set_offline 会翻 false） */
    return sd_spi_is_ready();
}
static bool     sd_backend_writable(void) {
#ifdef MSC_EXPORT_READONLY
    return false;
#else
    return true;   /* TF 卡无写保护开关可读（microSD 本就没有），恒可写 */
#endif
}

/* 直通策略：写请求立即落到卡（write-through）。
 * 为什么这里不做固件写缓存？TF 卡内部 FTL 已有整页缓存与掉电脆弱区，
 * 固件再叠一层缓存只会扩大"主机认为已写入、卡还没落盘"的窗口；
 * 数据安全靠主机在安全弹出时发 SYNCHRONIZE CACHE（本层收到即返回即可）。
 * 性能优化方向（商业级改造清单）：
 *   1) 核 1 做 N 块预读（read-ahead）：主机读 FAT 表/目录时提前把相邻扇区拉进环形缓冲，
 *      读10 回调立即返回缓存数据，IO 完成后 tud_msc_async_io_done() 通知栈；
 *   2) 按卡分配单元（AU，通常 4MB）对齐合并写；
 *   3) PIO-SDIO 4-bit 替代 SPI（RP2040 可到 10MB/s 级，社区 no-OS-FatFS 方案）。 */
static void sd_backend_read(uint32_t lba, uint8_t *buf, uint32_t nblk) {
    sd_spi_read_blocks(lba, buf, nblk);
    msc_io_ticks += nblk;
}
static void sd_backend_write(uint32_t lba, const uint8_t *buf, uint32_t nblk) {
    sd_spi_write_blocks(lba, buf, nblk);
    msc_io_ticks += nblk;
}
static void sd_backend_init_backend(void) {
    /* 卡上电初始化由 main.c 主循环做（可能耗时数百 ms，不能阻塞 USB 上下文） */
}
#endif /* MSC_USE_SD_CARD */

/*=========================== 后端统一接口 ===========================*/
static uint32_t disk_block_count(void) {
#ifdef MSC_USE_SD_CARD
    return sd_backend_block_count();
#else
    return ramdisk_block_count();
#endif
}
static bool disk_ready(void) {
#ifdef MSC_USE_SD_CARD
    return sd_backend_ready();
#else
    return ramdisk_ready();
#endif
}
static bool disk_writable(void) {
#ifdef MSC_USE_SD_CARD
    return sd_backend_writable();
#else
    return ramdisk_writable();
#endif
}
static void disk_read(uint32_t lba, uint8_t *buf, uint32_t nblk) {
#ifdef MSC_USE_SD_CARD
    sd_backend_read(lba, buf, nblk);
#else
    ramdisk_read(lba, buf, nblk);
#endif
}
static void disk_write(uint32_t lba, const uint8_t *buf, uint32_t nblk) {
#ifdef MSC_USE_SD_CARD
    sd_backend_write(lba, buf, nblk);
#else
    ramdisk_write(lba, buf, nblk);
#endif
}
static void disk_flush(void) {
#ifndef MSC_USE_SD_CARD
    ramdisk_flush();
#endif
}
void msc_app_flush(void) { disk_flush(); }   /* main.c 弹卡路径也会调用 */

/*=========================== TinyUSB MSC 回调 ===========================*/

/* 单 LUN（一个卡座）；打印机桥接读卡器等多槽位场景在此返回槽数 */
uint8_t tud_msc_get_maxlun_cb(void) { return 1; }

/* INQUIRY(0x12)：主机 lsusb/设备管理器里看到的厂商与产品名（各字段空格填充） */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void) lun;
    const char vid[] = "USB-Labs";
    const char pid[] = "MSC Disk";
    const char rev[] = "1.0 ";
    memcpy(vendor_id,   vid, sizeof(vid) <= 8  ? sizeof(vid)  : 8);
    memcpy(product_id,  pid, sizeof(pid) <= 16 ? sizeof(pid)  : 16);
    memcpy(product_rev, rev, sizeof(rev) <= 4  ? sizeof(rev)  : 4);
}

/* TEST UNIT READY(0x00)：主机所有操作前的心跳探测。
 * 返回 false 时 TinyUSB 自动 STALL 该命令，随后主机必发 REQUEST SENSE 取原因——
 * 我们在返回前把"病因"写进 Sense：NOT READY / 3Ah / 00h = Medium Not Present。
 * 该三元组就是读卡器空槽时 PC 转圈报错背后的字节（见 capture 文档错误场景）。 */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!disk_ready()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

/* READ CAPACITY(10)（0x25）：容量上报对接点，两个坑：
 *  1) block_count 上报的是"块数"，但规范里 READ CAPACITY 返回的 LAST LBA = 块数 - 1
 *     （TinyUSB 栈内部负责减一，本回调给块数即可）；
 *  2) 超过 2TB（块数溢出 32 位）时主机改发 READ CAPACITY(16)——RP2040 场景不会碰到，
 *     但做 eMMC 大盘产品时必须知道这条边界。 */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void) lun;
    *block_count = disk_block_count();
    *block_size  = DISK_BLOCK_SIZE;
}

/* START STOP UNIT（0x1B）：安全弹出的最后一环。
 * start=0, load_eject=1 → 主机要求"停转/退盘"。此时必须把缓存刷干净，
 * 之后断电才不丢数据（记录仪产品里这一拍还会顺便给卡断电，见 hardware/设计要点.md）。 */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void) lun; (void) power_condition;
    if (!start) disk_flush();       /* 停盘：把写缓存刷落介质 */
    return true;
}

/* 主机写之前会问"可写吗"（配合 WRITE10 前的只读检查）。
 * 行车记录仪导出模式就把这里关成 false：PC 只能拷不能删，防误操作。 */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void) lun;
    return disk_writable();
}

/* READ(10)（0x28）：数据阶段被 TinyUSB 按 CFG_TUD_MSC_EP_BUFSIZE 切片送进来。
 * 语义：把 [lba*512 + offset, +bufsize) 的介质内容拷进 buffer，返回实际提供的字节数。
 * 返回值协议：>0 已完成字节数（可小于 bufsize 分段续传）；0 = 忙，稍后重调；
 *            负数 = 出错（栈会 STALL 并进入失败路径，主机随后 REQUEST SENSE）。 */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    uint32_t const nblocks = disk_block_count();
    if (!disk_ready()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return -1;
    }
    if (lba >= nblocks) {
        /* ILLEGAL REQUEST / 21h / 00h = LOGICAL BLOCK ADDRESS OUT OF RANGE
         * （READ CAPACITY 报了容量，主机越界就打这个 Sense，capture 文档有完整帧） */
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
        return -1;
    }

    uint8_t *dst = (uint8_t *) buffer;
    /* bufsize 通常恰好 512/1024（=1~2 扇区，由 EP 缓冲决定），但仍按通用切片写法，
     * 对 offset 不对齐、跨扇区的请求都成立 */
    while (bufsize > 0) {
        uint32_t chunk = DISK_BLOCK_SIZE - offset;
        if (chunk > bufsize) chunk = bufsize;
        if (chunk == DISK_BLOCK_SIZE) {
            disk_read(lba, dst, 1);                  /* 整块：直读目标缓冲 */
        } else {
            disk_read(lba, s_stage, 1);              /* 部分：先入暂存再拷 */
            memcpy(dst, s_stage + offset, chunk);
        }
        dst     += chunk;
        offset  += chunk;
        if (offset >= DISK_BLOCK_SIZE) { offset = 0; lba++; }
        bufsize -= chunk;
    }
    return (int32_t) (dst - (uint8_t *) buffer);
}

/* WRITE(10)（0x2A）：方向与 READ10 相反——主机把介质数据推进来，我们落到块设备。
 * 注意 offset/lba 切片语义与 read10 完全对称；返回值协议同上。 */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    uint32_t const nblocks = disk_block_count();
    if (!disk_ready()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return -1;
    }
    if (!disk_writable()) {
        /* 主机明知只读还写（或多主机抢卡）→ DATA PROTECT / 27h / 00h */
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }
    if (lba >= nblocks) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00);
        return -1;
    }

    uint32_t done = 0;
    while (bufsize > 0) {
        uint32_t chunk = DISK_BLOCK_SIZE - offset;
        if (chunk > bufsize) chunk = bufsize;
        if (chunk == DISK_BLOCK_SIZE) {
            disk_write(lba, buffer + done, 1);       /* 整块：直写 */
        } else {
            /* 部分块写 = 读-改-写（read-modify-write）：
             * 先取原块，覆盖 [offset, offset+chunk)，再整体写回 */
            disk_read(lba, s_stage, 1);
            memcpy(s_stage + offset, buffer + done, chunk);
            disk_write(lba, s_stage, 1);
        }
        done    += chunk;
        offset  += chunk;
        if (offset >= DISK_BLOCK_SIZE) { offset = 0; lba++; }
        bufsize -= chunk;
    }
    return (int32_t) done;
}

/* WRITE10 收尾（数据阶段全部落库后）：教学后端不在此 flush（写缓存按 4KB 窗管理），
 * 真正的落盘时点：SYNCHRONIZE CACHE / START STOP(stop) / 换窗。 */
void tud_msc_write10_complete_cb(uint8_t lun) {
    (void) lun;
}

/* 不在 TinyUSB 内建清单里的命令落到这里。内建：INQUIRY / REQUEST SENSE /
 * READ CAPACITY(10) / READ FORMAT CAPACITIES / MODE SENSE(6) / READ10 / WRITE10。
 * 这里只处理两类有真实商业含义的： */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void) buffer; (void) bufsize;

    switch (scsi_cmd[0]) {
        case 0x1E:  /* PREVENT/ALLOW MEDIUM REMOVAL：Windows 挂盘/弹出的例行命令。
                     * scsi_cmd[4]&1 = 禁止拔卡；我们只有一个卡座、卡也不能被软件弹出，
                     * 记录即可（新版 TinyUSB 有独立弱回调，旧版落到这里，两处都兼容）。 */
            return 0;               /* 无数据阶段，返回 0 = 成功 */

        case 0x35: {  /* SYNCHRONIZE CACHE(10)：主机要求把易失缓存刷到介质。
                       * 这是"安全弹出"的数据承诺点：此后掉电主机不再为丢数据负责。 */
            disk_flush();
            return 0;
        }

        default:
            /* 未实现命令的标准回答：ILLEGAL REQUEST / 20h / 00h
             * （Invalid Command Operation Code）。不要静默成功——主机缓存策略会失真。 */
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

/*=========================== 供 main.c 调用的后端初始化 ===========================*/
void msc_app_init(void) {
#ifdef MSC_USE_SD_CARD
    sd_detect_init();
#else
    ramdisk_init_backend();
#endif
}

/* 主循环里的卡热插拔处理（SD 后端）：只在主循环做，绝不在 USB 回调里做——
 * sd_spi_init 可能耗时数百 ms，阻塞会拖垮 BOT 状态机超时。 */
void msc_app_task(void) {
#ifdef MSC_USE_SD_CARD
    static bool last_present = false;
    static uint32_t debounce_t0 = 0;
    bool present = sd_card_present();
    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (present != last_present) {
        debounce_t0 = now;                 /* 状态翻转，进入去抖观察窗 */
        last_present = present;
    } else if (now - debounce_t0 > 50) {   /* 50ms 稳定才认账（触点弹跳毫秒级） */
        static bool inited = false;
        if (present && !inited) {
            inited = sd_spi_init();        /* 插卡：全初始化（400kHz→12.5MHz） */
        } else if (!present && inited) {
            sd_spi_set_offline();          /* 拔卡：MSC 层立刻回 NOT READY/3Ah */
            inited = false;
        }
    }
#endif
}

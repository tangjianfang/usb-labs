/* Lab3 MSC —— SPI 模式 TF 卡驱动
 *
 * 协议依据：SD Association《SD Physical Layer Simplified Specification》
 * （第 7 章 SPI 模式；CSD 字段见 5.6 节）。教学实现要点：
 *  - 只需 7 条命令：CMD0（复位）、CMD8（电压检测）、CMD55+ACMD41（初始化）、
 *    CMD58（OCR/CCS 判 SDHC）、CMD59（关 CRC）、CMD9（CSD→容量）、CMD16/17/24（块读写）
 *  - 只有 CMD0/CMD8 需要合法 CRC（初值固定 0x95/0x87，SD 规范给定的已知值），
 *    之后发 CMD59 关 CRC，其余命令 CRC 字节填 0x01 即可——这也是极简 SD 驱动的通行做法
 *  - SDHC/SDXC（≥2GB 的事实主流）使用 512B 扇区号寻址；SDSC 用字节地址（左移 9 位）
 *
 * 提升空间（量产方向）：CMD18/25 多块读写 + DMA、PIO 搬运、核 1 后台调度——
 * 见 msc_app.c 顶部"性能路线"注释。
 */
#include "sd_spi.h"

#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"

/*------------------------- 内部状态 -------------------------*/
static bool     s_ready;        /* 卡完成初始化、可块读写 */
static bool     s_sdhc;         /* SDHC/SDXC：扇区寻址 */
static uint32_t s_sectors;      /* 总扇区数（512B/扇区） */

/*------------------------- SPI 底层 -------------------------*/
static inline uint8_t xfer(uint8_t tx) {
    uint8_t rx;
    spi_write_read_blocking(SD_SPI_PORT, &tx, &rx, 1);
    return rx;
}

static inline void cs_low(void)  { gpio_put(SD_SPI_CS_PIN, 0); }
static inline void cs_high(void) { gpio_put(SD_SPI_CS_PIN, 1); }

/* 卡片选释放后必须补 8 个时钟（卡在 MISO 上结束输出，规范 NRC/NCR 时序） */
static inline void cs_release(void) {
    cs_high();
    xfer(0xFF);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/*------------------------- 命令层 -------------------------*/
/* 发命令并取 R1 响应（bit0..6 状态，bit7=0 表示响应有效）。
 * resp 可为 NULL；cmd==8 时会额外回读 4 字节 R7 payload。返回 0xFF 表示超时。 */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t resp[4]) {
    uint8_t frame[6] = {
        (uint8_t) (0x40u | cmd),
        (uint8_t) (arg >> 24), (uint8_t) (arg >> 16),
        (uint8_t) (arg >> 8),  (uint8_t) (arg),
        0x01,                                  /* CRC 占位 + 结束位 */
    };
    /* 仅两条命令需要真 CRC（规范给定的固定值），其余靠 CMD59 关 CRC */
    if (cmd == 0) frame[5] = 0x95;
    if (cmd == 8) frame[5] = 0x87;

    xfer(0xFF);                                /* 命令前导：至少 1 字节 */
    cs_low();
    spi_write_blocking(SD_SPI_PORT, frame, sizeof(frame));

    /* R1：等待 bit7=0 的字节（最多约 8 字节 Ncr + 余量） */
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10; i++) {
        r1 = xfer(0xFF);
        if ((r1 & 0x80u) == 0) break;
    }
    if ((r1 & 0x80u) == 0 && resp != NULL) {
        resp[0] = xfer(0xFF);
        resp[1] = xfer(0xFF);
        resp[2] = xfer(0xFF);
        resp[3] = xfer(0xFF);
    }
    cs_release();
    return r1;
}

static inline uint8_t sd_acmd(uint8_t cmd, uint32_t arg, uint8_t resp[4]) {
    uint8_t r1 = sd_send_cmd(55, 0, NULL);     /* ACMD = CMD55 + CMD */
    if (r1 > 1) return r1;
    return sd_send_cmd(cmd, arg, resp);
}

/*------------------------- 初始化 -------------------------*/
void sd_detect_init(void) {
    if (SD_CD_PIN >= 0) {
        gpio_init(SD_CD_PIN);
        gpio_set_dir(SD_CD_PIN, GPIO_IN);
#if SD_CD_ACTIVE_LOW
        gpio_pull_up(SD_CD_PIN);
#else
        gpio_pull_down(SD_CD_PIN);
#endif
    }
}

bool sd_card_present(void) {
    if (SD_CD_PIN < 0) return true;            /* 无 CD 脚：上层用命令探测兜底 */
    bool raw = gpio_get(SD_CD_PIN);
#if SD_CD_ACTIVE_LOW
    return !raw;                               /* 低电平 = 有卡 */
#else
    return raw;
#endif
}

/* 读单块（含 0xFE 数据令牌），初始化期读 CSD 用 */
static bool sd_read_data(uint8_t token, uint8_t *buf, uint32_t len) {
    uint32_t t0 = now_ms();
    while (xfer(0xFF) != token) {              /* 等数据令牌（卡忙时回 0x00） */
        if (now_ms() - t0 > 200) return false;
    }
    spi_read_blocking(SD_SPI_PORT, 0xFF, buf, (size_t) len);
    xfer(0xFF); xfer(0xFF);                    /* 丢 CRC16 */
    return true;
}

bool sd_spi_init(void) {
    s_ready = false; s_sdhc = false; s_sectors = 0;
    if (!sd_card_present()) return false;

    /* 1) 低速 400kHz 起步（卡初始化期间有严格时钟上限），并配好 CS */
    spi_init(SD_SPI_PORT, 400 * 1000);
    gpio_set_function(SD_SPI_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(SD_SPI_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_SPI_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(SD_SPI_CS_PIN);
    gpio_set_dir(SD_SPI_CS_PIN, GPIO_OUT);
    cs_release();

    /* 2) 上电至少 74 个时钟，让卡内部电荷泵稳定（这里给 80） */
    for (int i = 0; i < 10; i++) xfer(0xFF);

    /* 3) CMD0 进 IDLE，个别卡需要重试 */
    uint8_t r1 = 0xFF;
    uint32_t t0 = now_ms();
    do {
        r1 = sd_send_cmd(0, 0, NULL);
        if (r1 == 0x01) break;
    } while (now_ms() - t0 < 200);
    if (r1 != 0x01) { cs_release(); return false; }

    /* 4) CMD8(0x1AA)：合法应答 = SD v2 物理规范卡；非法命令应答(0x05) = v1 老卡 */
    uint8_t ifcond[4] = {0};
    r1 = sd_send_cmd(8, 0x1AA, ifcond);
    bool v2 = (r1 == 0x01 && ifcond[2] == 0x01 && ifcond[3] == 0xAA);

    /* 5) ACMD41：等卡出 IDLE（HCS=bit30 告知主机支持 SDHC）；实测上限 1s 足够 */
    t0 = now_ms();
    do {
        r1 = sd_acmd(41, v2 ? (1ul << 30) : 0, NULL);
        if (r1 == 0x00) break;
    } while (now_ms() - t0 < 1000);
    if (r1 != 0x00) { cs_release(); return false; }

    /* 6) CMD58 读 OCR：bit30(CCS)=1 → SDHC/SDXC（512B 扇区寻址） */
    uint8_t ocr[4] = {0};
    if (sd_send_cmd(58, 0, ocr) > 1) { cs_release(); return false; }
    s_sdhc = (ocr[0] & 0x40u) != 0;

    /* 7) 关 CRC 校验（后面命令/数据的 CRC 字节都填哑值） */
    sd_send_cmd(59, 0, NULL);

    /* 8) CMD16：定块长 512（SDHC 固定 512，此命令无害） */
    if (sd_send_cmd(16, SD_BLOCK_SIZE, NULL) > 2) { cs_release(); return false; }

    /* 9) CMD9 读 CSD → 解析容量（对接 tud_msc_capacity_cb 的数据源头） */
    uint8_t csd[16] = {0};
    cs_low();
    if (sd_send_cmd(9, 0, NULL) != 0x00 || !sd_read_data(0xFE, csd, 16)) {
        cs_release(); return false;
    }
    cs_release();

    if ((csd[0] >> 6) == 0x01) {               /* CSD v2（SDHC/SDXC） */
        uint32_t csize = ((uint32_t) (csd[7] & 0x3F) << 16)
                       | ((uint32_t) csd[8] << 8) | csd[9];
        s_sectors = (csize + 1) << 10;         /* 容量 = (C_SIZE+1) × 512KB */
    } else {                                   /* CSD v1（SDSC） */
        uint8_t  rd_bl_len  = csd[5] & 0x0F;
        uint16_t c_size     = (uint16_t) (((csd[6] & 0x03) << 10) | ((uint16_t) csd[7] << 2) | (csd[8] >> 6));
        uint8_t  c_mult     = (uint8_t) (((csd[9] & 0x03) << 1) | (csd[10] >> 7));
        uint32_t blocks     = ((uint32_t) c_size + 1) << (c_mult + 2);
        s_sectors = blocks << (rd_bl_len - 9);
    }
    if (s_sectors == 0) { cs_release(); return false; }

    /* 10) 提速到 12.5MHz（125MHz 外设时钟 / 10；SD SPI 模式上限约 25MHz） */
    spi_set_baudrate(SD_SPI_PORT, 12 * 1000 * 1000);

    s_ready = true;
    return true;
}

void sd_spi_set_offline(void) { s_ready = false; }
bool sd_spi_is_ready(void)    { return s_ready; }
uint32_t sd_spi_sector_count(void) { return s_ready ? s_sectors : 0; }

/*------------------------- 块读写 -------------------------*/
static bool sd_read_one(uint32_t lba, uint8_t *buf) {
    uint32_t addr = s_sdhc ? lba : (lba << 9); /* SDSC 是字节地址 */
    if (sd_send_cmd(17, addr, NULL) != 0x00) return false;
    cs_low();
    bool ok = sd_read_data(0xFE, buf, SD_BLOCK_SIZE);
    cs_release();
    return ok;
}

static bool sd_write_one(uint32_t lba, const uint8_t *buf) {
    uint32_t addr = s_sdhc ? lba : (lba << 9);
    if (sd_send_cmd(24, addr, NULL) != 0x00) return false;

    cs_low();
    xfer(0xFF);                                /* Nwr 前导 */
    xfer(0xFE);                                /* 单块写数据令牌 */
    spi_write_blocking(SD_SPI_PORT, buf, SD_BLOCK_SIZE);
    xfer(0xFF); xfer(0xFF);                    /* 哑 CRC16 */

    /* 数据响应令牌：xxx00101 = 接受 */
    uint32_t t0 = now_ms();
    uint8_t resp;
    do {
        resp = xfer(0xFF) & 0x1F;
        if (now_ms() - t0 > 100) { cs_release(); return false; }
    } while (resp != 0x05);

    /* 卡内部烧写期拉低 DAT0（busy），等它回高（大页卡实测可达数百 ms） */
    t0 = now_ms();
    while (xfer(0xFF) == 0x00) {
        if (now_ms() - t0 > 500) { cs_release(); return false; }
    }
    cs_release();
    return true;
}

bool sd_spi_read_blocks(uint32_t lba, uint8_t *buf, uint32_t nblocks) {
    if (!s_ready) return false;
    for (uint32_t i = 0; i < nblocks; i++)
        if (!sd_read_one(lba + i, buf + i * SD_BLOCK_SIZE)) return false;
    return true;   /* 量产优化：换 CMD18 多块读 + DMA，吞吐可翻倍以上 */
}

bool sd_spi_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t nblocks) {
    if (!s_ready) return false;
    for (uint32_t i = 0; i < nblocks; i++)
        if (!sd_write_one(lba + i, buf + i * SD_BLOCK_SIZE)) return false;
    return true;   /* 量产优化：CMD25 多块写 + 按卡 AU 对齐合并写 */
}

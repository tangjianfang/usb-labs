/* Lab3 MSC —— SPI 模式 TF 卡驱动（对外接口）
 *
 * 职责边界：本驱动只负责"块设备"语义（读/写 512B 扇区、容量、在位检测），
 * 不含文件系统——文件系统由主机（PC）负责，固件只是块搬运工（这正是 MSC 的本质）。
 *
 * 引脚等配置全部可在编译期覆盖（cmake -Dxxx=yyy 或 target_compile_definitions）。
 */
#ifndef LAB3_SD_SPI_H_
#define LAB3_SD_SPI_H_

#include <stdint.h>
#include <stdbool.h>

/*------------------------- 编译期配置 -------------------------*/
#ifndef SD_SPI_PORT
#define SD_SPI_PORT          spi1
#endif
#ifndef SD_SPI_SCK_PIN
#define SD_SPI_SCK_PIN       10       /* RP2040 spi1 SCK */
#endif
#ifndef SD_SPI_MOSI_PIN
#define SD_SPI_MOSI_PIN      11       /* spi1 TX（接卡座 CMD） */
#endif
#ifndef SD_SPI_MISO_PIN
#define SD_SPI_MISO_PIN      12       /* spi1 RX（接卡座 DAT0） */
#endif
#ifndef SD_SPI_CS_PIN
#define SD_SPI_CS_PIN        13       /* 普通 GPIO 片选（接卡座 DAT3/CS） */
#endif
#ifndef SD_CD_PIN
#define SD_CD_PIN            9        /* 卡座 Card Detect 开关脚；无此脚设为负值并自行兜底 */
#endif
/* CD 极性：1 = 无卡时闭合到地（低电平=无卡），2 种座子都有，按实测改 */
#ifndef SD_CD_ACTIVE_LOW
#define SD_CD_ACTIVE_LOW     1
#endif

#define SD_BLOCK_SIZE        512u

/*------------------------- API -------------------------*/

/* 初始化 CD 检测脚（GPIO 去抖由调用方在主循环做） */
void sd_detect_init(void);

/* 卡是否物理在位（读 CD 脚；SD_CD_PIN<0 时恒返回 true，需上层兜底探测） */
bool sd_card_present(void);

/* 上电初始化卡（CMD0→CMD8→ACMD41→CMD58→CMD9），成功后切换高速时钟。
 * 耗时可达数百 ms，只允许在主循环上下文调用，禁止在 USB 回调/中断里调用。 */
bool sd_spi_init(void);

/* 卡被拔出时调用：使驱动回到"未初始化"态（MSC 层据此回 NOT READY） */
void sd_spi_set_offline(void);

/* 驱动是否处于可用状态 */
bool sd_spi_is_ready(void);

/* 总扇区数（来自 CSD 寄存器，SDHC/SDXC 为 (C_SIZE+1)<<10） */
uint32_t sd_spi_sector_count(void);

/* 块读写：块地址恒为 512B 扇区号（SDHC/SDXC 原生寻址；SDSC 内部换算字节地址） */
bool sd_spi_read_blocks(uint32_t lba, uint8_t *buf, uint32_t nblocks);
bool sd_spi_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t nblocks);

#endif /* LAB3_SD_SPI_H_ */

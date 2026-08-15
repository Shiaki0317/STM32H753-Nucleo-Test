#ifndef W25Q64JV_QSPI_H
#define W25Q64JV_QSPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32h7xx_hal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * W25Q64JV:
 *   Capacity    : 64 Mbit = 8 MiB
 *   Page size   : 256 bytes
 *   Sector size : 4 KiB
 *   Address     : 24 bits
 *
 * The complete 8 MiB address space is accessed with 24-bit addresses.
 */
#define W25Q64JV_FLASH_SIZE_BYTES       (8UL * 1024UL * 1024UL)
#define W25Q64JV_PAGE_SIZE_BYTES        256UL
#define W25Q64JV_SECTOR_SIZE_BYTES      4096UL
#define W25Q64JV_MEMORY_MAPPED_BASE     0x90000000UL
#define W25Q64JV_MEMORY_MAPPED_END      \
    (W25Q64JV_MEMORY_MAPPED_BASE + W25Q64JV_FLASH_SIZE_BYTES - 1UL)

typedef struct
{
    uint8_t manufacturer_id;
    uint8_t memory_type;
    uint8_t capacity_id;
} W25Q64JV_JedecId;

/*
 * Initializes the device, verifies the JEDEC ID, and enables Quad mode.
 * Call MX_QUADSPI_Init() before this function.
 */
HAL_StatusTypeDef W25Q64JV_Quad_Init(QSPI_HandleTypeDef *hqspi);

/* Performs 66h + 99h software reset in 1-line SPI mode. */
HAL_StatusTypeDef W25Q64JV_Quad_Reset(QSPI_HandleTypeDef *hqspi);

/* Reads the three-byte JEDEC ID with command 9Fh. */
HAL_StatusTypeDef W25Q64JV_Quad_ReadJedecId(
    QSPI_HandleTypeDef *hqspi,
    W25Q64JV_JedecId *id);

/* Accepts W25Q64JV-IQ (EF 40 17) and W25Q64JV-IM (EF 70 17). */
bool W25Q64JV_Quad_IsSupportedId(const W25Q64JV_JedecId *id);

/* Reads Status Register-1 or Status Register-2. */
HAL_StatusTypeDef W25Q64JV_Quad_ReadStatus1(
    QSPI_HandleTypeDef *hqspi,
    uint8_t *status);

HAL_StatusTypeDef W25Q64JV_Quad_ReadStatus2(
    QSPI_HandleTypeDef *hqspi,
    uint8_t *status);

/*
 * Reads arbitrary bytes using Fast Read Quad Output (6Bh).
 * Instruction: 1 line, address: 1 line/24 bits, data: 4 lines.
 */
HAL_StatusTypeDef W25Q64JV_Quad_Read(
    QSPI_HandleTypeDef *hqspi,
    uint32_t address,
    uint8_t *data,
    size_t length);

/*
 * Programs arbitrary bytes by splitting them at 256-byte page boundaries.
 *
 * Important:
 *   - The destination must already be erased.
 *   - NOR flash programming only changes bits from 1 to 0.
 *   - This function does not erase sectors automatically.
 */
HAL_StatusTypeDef W25Q64JV_Quad_Program(
    QSPI_HandleTypeDef *hqspi,
    uint32_t address,
    const uint8_t *data,
    size_t length);

/*
 * Erases one 4-KiB sector.
 * address must be aligned to W25Q64JV_SECTOR_SIZE_BYTES.
 */
HAL_StatusTypeDef W25Q64JV_Quad_EraseSector4K(
    QSPI_HandleTypeDef *hqspi,
    uint32_t address);

/*
 * Erases every 4-KiB sector that overlaps the requested range.
 * address and length do not need to be sector-aligned.
 */
HAL_StatusTypeDef W25Q64JV_Quad_EraseRange4K(
    QSPI_HandleTypeDef *hqspi,
    uint32_t address,
    size_t length);

/*
 * Enables read-only memory-mapped access at 0x90000000.
 * Program and erase operations require leaving memory-mapped mode first.
 */
HAL_StatusTypeDef W25Q64JV_Quad_EnableMemoryMapped(
    QSPI_HandleTypeDef *hqspi);

/* Leaves memory-mapped mode by aborting the QSPI peripheral. */
HAL_StatusTypeDef W25Q64JV_Quad_DisableMemoryMapped(
    QSPI_HandleTypeDef *hqspi);

#ifdef __cplusplus
}
#endif

#endif /* W25Q64JV_QSPI_H */

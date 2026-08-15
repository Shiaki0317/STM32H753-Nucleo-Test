/*
 * w25q256jv.h
 *
 *  Created on: 2026/07/21
 *      Author: shidaa
 */

#ifndef INC_W25Q64JV_H_
#define INC_W25Q64JV_H_

#include "user_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define W25Q64JV_SIZE_BYTES        (8UL * 1024UL * 1024UL)
#define W25Q64JV_PAGE_SIZE         256UL
#define W25Q64JV_SECTOR_SIZE       4096UL

/**
 * @brief Flashをリセットし、JEDEC IDを確認します。
 *
 * jedec_idには3バイトのIDが格納されます。
 * 通常品では EF 40 17 が期待値です。
 */
HAL_StatusTypeDef W25Q64JV_Init(uint8_t jedec_id[3]);

HAL_StatusTypeDef W25Q64JV_Reset(void);

HAL_StatusTypeDef W25Q64JV_ReadJedecId(uint8_t jedec_id[3]);

HAL_StatusTypeDef W25Q64JV_ReadStatus1(uint8_t *status);

HAL_StatusTypeDef W25Q64JV_WaitReady(uint32_t timeout_ms);

/**
 * @brief 指定した4KBセクタを消去します。
 *
 * addressはセクタ先頭に自動的に切り下げられます。
 */
HAL_StatusTypeDef W25Q64JV_EraseSector(uint32_t address);

/**
 * @brief 最大256バイトを1ページ内に書き込みます。
 *
 * ページ境界をまたぐことはできません。
 */
HAL_StatusTypeDef W25Q64JV_ProgramPage(
    uint32_t address,
    const uint8_t *data,
    uint32_t length);

/**
 * @brief ページ境界を考慮して連続書込みします。
 *
 * この関数は消去処理を行いません。
 */
HAL_StatusTypeDef W25Q64JV_Write(
    uint32_t address,
    const uint8_t *data,
    uint32_t length);

HAL_StatusTypeDef W25Q64JV_Read(
    uint32_t address,
    uint8_t *data,
    uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* INC_W25Q64JV_H_ */

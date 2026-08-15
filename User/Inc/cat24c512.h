/*
 * cat24c512.h
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#ifndef INC_CAT24C512_H_
#define INC_CAT24C512_H_

#include "user_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAT24C512_TOTAL_SIZE          65536UL
#define CAT24C512_PAGE_SIZE           128UL

/*
 * A2=A1=A0=Lowの場合:
 * 7bitアドレス = 0x50
 * STM32 HALへ渡す値 = 0x50 << 1 = 0xA0
 */
#define CAT24C512_DEVICE_ADDRESS      (0x50U << 1U)

#define CAT24C512_I2C_TIMEOUT_MS      100U
#define CAT24C512_READY_TIMEOUT_MS    10U

HAL_StatusTypeDef CAT24C512_IsReady(I2C_HandleTypeDef *hi2c);

HAL_StatusTypeDef CAT24C512_Read(I2C_HandleTypeDef *hi2c, uint16_t address, uint8_t *data, uint32_t length);

HAL_StatusTypeDef CAT24C512_Write(I2C_HandleTypeDef *hi2c, uint16_t address, const uint8_t *data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* INC_CAT24C512_H_ */

/*
 * user_util.h
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#ifndef INC_USER_UTIL_H_
#define INC_USER_UTIL_H_

#include "stm32h7xx_hal.h"
#include "stm32h7xx_nucleo.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

uint8_t* get_status(HAL_StatusTypeDef stat, uint8_t* result, uint16_t length);

#endif /* INC_USER_UTIL_H_ */

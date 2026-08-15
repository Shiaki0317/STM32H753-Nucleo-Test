/*
 * cat24c512.c
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#include "cat24c512.h"

#define CAT24C512_READ_CHUNK_SIZE    1024U

static uint32_t CAT24C512_Min(uint32_t a, uint32_t b) {
	return (a < b) ? a : b;
}

/*
 * EEPROM内部の書き込み完了をACKポーリングで待つ。
 *
 * CAT24C512は内部書き込み中、スレーブアドレスにACKを返さない。
 */
static HAL_StatusTypeDef CAT24C512_WaitReady(I2C_HandleTypeDef *hi2c, uint32_t timeout_ms) {
	uint32_t start_tick;

	if (hi2c == NULL) {
		return HAL_ERROR;
	}

	start_tick = HAL_GetTick();

	while ((HAL_GetTick() - start_tick) < timeout_ms) {
		HAL_StatusTypeDef status;

		status = HAL_I2C_IsDeviceReady(hi2c, CAT24C512_DEVICE_ADDRESS, 1U, 2U);

		if (status == HAL_OK) {
			return HAL_OK;
		}
	}

	return HAL_TIMEOUT;
}

HAL_StatusTypeDef CAT24C512_IsReady(I2C_HandleTypeDef *hi2c) {
	if (hi2c == NULL) {
		return HAL_ERROR;
	}

	return HAL_I2C_IsDeviceReady(hi2c, CAT24C512_DEVICE_ADDRESS, 3U, CAT24C512_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef CAT24C512_Read(I2C_HandleTypeDef *hi2c, uint16_t address, uint8_t *data, uint32_t length) {
	uint32_t current_address;
	uint32_t remaining;
	uint8_t *current_data;

	if (hi2c == NULL) {
		return HAL_ERROR;
	}

	if ((data == NULL) && (length != 0U)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	/*
	 * addressはuint16_tだが、加算時のオーバーフローを避けるため
	 * uint32_tへ変換して範囲を確認する。
	 */
	if (((uint32_t)address + length) > CAT24C512_TOTAL_SIZE) {
		return HAL_ERROR;
	}

	current_address = address;
	current_data = data;
	remaining = length;

	while (remaining > 0U) {
		uint32_t chunk_size;
		HAL_StatusTypeDef status;

		chunk_size = CAT24C512_Min(remaining, CAT24C512_READ_CHUNK_SIZE);
		status = HAL_I2C_Mem_Read(hi2c, CAT24C512_DEVICE_ADDRESS, (uint16_t)current_address, I2C_MEMADD_SIZE_16BIT, current_data, (uint16_t)chunk_size, CAT24C512_I2C_TIMEOUT_MS);

		if (status != HAL_OK) {
			return status;
		}

		current_address += chunk_size;
		current_data += chunk_size;
		remaining -= chunk_size;
	}

	return HAL_OK;
}

HAL_StatusTypeDef CAT24C512_Write(I2C_HandleTypeDef *hi2c, uint16_t address, const uint8_t *data, uint32_t length) {
	uint32_t current_address;
	uint32_t remaining;
	const uint8_t *current_data;

	if (hi2c == NULL) {
		return HAL_ERROR;
	}

	if ((data == NULL) && (length != 0U)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	if (((uint32_t)address + length) > CAT24C512_TOTAL_SIZE) {
		return HAL_ERROR;
	}

	current_address = address;
	current_data = data;
	remaining = length;

	while (remaining > 0U) {
		uint32_t page_offset;
		uint32_t page_remaining;
		uint32_t chunk_size;
		HAL_StatusTypeDef status;

		/*
		 * 現在アドレスがページ内のどこにあるかを計算する。
		 */
		page_offset = current_address % CAT24C512_PAGE_SIZE;

		/*
		 * 現在ページの末尾までに書けるバイト数。
		 */
		page_remaining = CAT24C512_PAGE_SIZE - page_offset;
		chunk_size = CAT24C512_Min(remaining, page_remaining);
		status = HAL_I2C_Mem_Write(hi2c, CAT24C512_DEVICE_ADDRESS, (uint16_t)current_address, I2C_MEMADD_SIZE_16BIT, (uint8_t *)current_data, (uint16_t)chunk_size, CAT24C512_I2C_TIMEOUT_MS);

		if (status != HAL_OK) {
			return status;
		}

		/*
		 * 固定のHAL_Delay(5)ではなく、ACKが返るまで待つ。
		 */
		status = CAT24C512_WaitReady(hi2c, CAT24C512_READY_TIMEOUT_MS);

		if (status != HAL_OK) {
			return status;
		}

		current_address += chunk_size;
		current_data += chunk_size;
		remaining -= chunk_size;
	}

	return HAL_OK;
}

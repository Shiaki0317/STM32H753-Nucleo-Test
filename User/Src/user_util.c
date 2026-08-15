/*
 * user_util.c
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#include "user_util.h"

uint8_t* get_status(HAL_StatusTypeDef stat, uint8_t* result, uint16_t length) {
	if (length < 20) {
		return NULL;
	}

	switch (stat) {
		case HAL_OK:
			memcpy(result, "OK!", length-strlen((char *)result)-1);
			break;
		case HAL_ERROR:
			memcpy(result, "ERROR!", length-strlen((char *)result)-1);
			break;
		case HAL_BUSY:
			memcpy(result, "BUSY!", length-strlen((char *)result)-1);
			break;
		case HAL_TIMEOUT:
			memcpy(result, "TIMEOUT!", length-strlen((char *)result)-1);
			break;
		default:
			memcpy(result, "NONE!", length-strlen((char *)result)-1);
			break;
	}
	return result;
}

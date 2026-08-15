/*
 * command_parser.c
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#include "command_parser.h"
#include "uart_receive.h"
#include "cat24c512.h"
#include "w25q64jv.h"
#include "user_util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define COMMAND_BUFFER_SIZE 64U
#define W25Q_TEST_ADDRESS  0x00100000UL

static char command_buffer[COMMAND_BUFFER_SIZE];
static uint16_t command_length;

static PacketParser packet_parser;

static void ProcessCommand(const char *command);
static void I2C_ProcessModeCommand(const char *command);
static void SPI_ProcessModeCommand(const char *command);
static void QSPI_ProcessModeCommand(const char *command);

void CommandParser_Init(void) {
	command_length = 0U;
	memset(command_buffer, 0, sizeof(command_buffer));
}

/**
 * @brief mainループから繰り返し呼び出す
 */
void CommandParser_Execute(void)
{
	uint8_t received_byte;

	while (UART_Receive_GetByte(&received_byte)) {
		switch (received_byte) {
			case '\b':
				/*
				 * BSは削除する。
				 */
				command_buffer[--command_length] = '\0';
				break;

			case '\r':
				/*
				 * CRは無視する。
				 */
				break;

			case '\n':
				/*
				 * LFを受信したら1コマンド確定
				 */
				if (command_length > 0U) {
					command_buffer[command_length] = '\0';

					ProcessCommand(command_buffer);

					command_length = 0U;
					command_buffer[0] = '\0';
					memset(command_buffer, 0, sizeof(command_buffer));
				}
				break;

			default:
				if (command_length < (COMMAND_BUFFER_SIZE - 1U)) {
					command_buffer[command_length] = (char)received_byte;
					command_length++;
				} else {
					/*
					 * コマンドが長すぎる場合は破棄する。
					 */
					command_length = 0U;
					command_buffer[0] = '\0';
				}
				break;
		}
	}
}

/**
 * @brief 受信コマンドによって処理を切り替える
 */
static void ProcessCommand(const char *command) {
	if (strcmp(command, "LED ON") == 0) {
		HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_SET);
	} else if (strcmp(command, "LED OFF") == 0) {
		HAL_GPIO_WritePin(GPIOG, GPIO_PIN_1, GPIO_PIN_RESET);
	} else if (strcmp(command, "STATUS") == 0) {
		/*
		 * 状態送信要求を登録する。
		 * 実際にはここでフラグを立てて、
		 * 別の処理で応答を送る方法もある。
		 */
	} else if (strncmp(command, "I2C MODE ", 9U) == 0) {
		I2C_ProcessModeCommand(command);
	} else if (strncmp(command, "SPI MODE ", 9U) == 0) {
		SPI_ProcessModeCommand(command);
	} else if (strncmp(command, "QSPI MODE ", 10U) == 0) {
		QSPI_ProcessModeCommand(command);
	} else {
		/*
		 * 未定義コマンド
		 */
	}
}

static void I2C_ProcessModeCommand(const char *command) {
	long mode = 0;
	char *end_pointer = NULL;
	uint8_t send_command_data[SEND_COMMAND_DATA] = "";
	uint8_t temp[SEND_COMMAND_DATA] = "";
	HAL_StatusTypeDef status = HAL_ERROR;

	uint8_t send_buffer[SEND_DATA] = "";
	uint8_t recv_buffer[RECV_DATA] = "";
	uint16_t loop_cnt = 0, page_loop_cnt = 0;

	mode = strtol(&command[9], &end_pointer, 10);

	/*
	 * 数字以外が含まれていないことを確認する。
	 */
	if (*end_pointer != '\0') {
		return;
	}

	switch ((i2c_command_mode)mode) {
		case I2C_ERROR_MODE:
			strncpy((char *)send_command_data, "(i2c)MODE : ERROR", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_CHECK_MODE:
			status = CAT24C512_IsReady(&hi2c2);
			strncat((char *)send_command_data, "(i2c)", SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);
			get_status(status, temp, SEND_COMMAND_DATA);
			strncat((char *)send_command_data, (char *)temp, SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_READ_WRITE_TEST:
			strncat((char *)send_command_data, "(i2c)", SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);

			strncpy((char *)send_buffer, "", SEND_DATA - strlen((char *)send_buffer) - 1);
			CAT24C512_Write(&hi2c2, 0, send_buffer, SEND_DATA);
			CAT24C512_Read(&hi2c2, 0, recv_buffer, RECV_DATA);
			strncat((char *)send_command_data, (char *)recv_buffer, SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);

			strncpy((char *)send_buffer, " / I2C_READ_WRITE_TEST", SEND_DATA - strlen((char *)send_buffer) - 1);
			CAT24C512_Write(&hi2c2, 0, send_buffer, SEND_DATA);
			CAT24C512_Read(&hi2c2, 0, recv_buffer, RECV_DATA);
			strncat((char *)send_command_data, (char *)recv_buffer, SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);

			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_READ_HEAD:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) \r\n", 10, 100);
			CAT24C512_Read(&hi2c2, 0, recv_buffer, RECV_DATA);

			for (page_loop_cnt = 0; page_loop_cnt < 128; page_loop_cnt++) {
				snprintf((char *)temp, SEND_COMMAND_DATA, "%02x ", recv_buffer[page_loop_cnt]);
				strncat((char *)send_command_data, (char *)temp, SEND_COMMAND_DATA-strlen((char *)temp)-1);
			}
			strncat((char *)send_command_data, (char *)"\r\n", SEND_COMMAND_DATA - strlen((char *)send_command_data) - 1);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			break;

		case I2C_READ_ALL:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) \r\n", 10, 100);
			for(loop_cnt = 0; loop_cnt < (512 * 1024 / 128); loop_cnt++) {
				CAT24C512_Read(&hi2c2, 128 * loop_cnt, recv_buffer, RECV_DATA);

				snprintf((char *)send_command_data, SEND_COMMAND_DATA, (char *)"%4d : ", loop_cnt);
				for (page_loop_cnt = 0; page_loop_cnt < 128; page_loop_cnt++) {
					snprintf((char *)temp, SEND_COMMAND_DATA, "%02x ", recv_buffer[page_loop_cnt]);
					strncat((char *)send_command_data, (char *)temp, SEND_COMMAND_DATA-strlen((char *)send_command_data)-1);
				}
				strncat((char *)send_command_data, (char *)"\r\n", SEND_COMMAND_DATA - strlen((char *)send_command_data) - 1);
				HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			}
			break;

		case I2C_WRITE_ALL_ZERO:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) ", 7, 100);
			memset(send_buffer, 0, SEND_DATA);
			for(loop_cnt = 0; loop_cnt < (512 * 1024 / 128); loop_cnt++) {
				CAT24C512_Write(&hi2c2, 128 * loop_cnt, send_buffer, SEND_DATA);
				if (loop_cnt % 500 == 0) {
					snprintf((char *)send_command_data, SEND_COMMAND_DATA, "%d, ", loop_cnt);
					HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
				}
			}
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			strncpy((char *)send_command_data, "(i2c) WRITE FIN -- ALL ZERO", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_WRITE_ALL_ONE:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) ", 7, 100);
			memset(send_buffer, 0xff, SEND_DATA);
			for(loop_cnt = 0; loop_cnt < (512 * 1024 / 128); loop_cnt++) {
				CAT24C512_Write(&hi2c2, 128 * loop_cnt, send_buffer, SEND_DATA);
				if (loop_cnt % 500 == 0) {
					snprintf((char *)send_command_data, SEND_COMMAND_DATA, "%d, ", loop_cnt);
					HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
				}
			}
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			strncpy((char *)send_command_data, "(i2c) WRITE FIN -- ALL ZERO", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_WRITE_ALL_5A:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) ", 7, 100);
			memset(send_buffer, 0x5a, SEND_DATA);
			for(loop_cnt = 0; loop_cnt < (512 * 1024 / 128); loop_cnt++) {
				CAT24C512_Write(&hi2c2, 128 * loop_cnt, send_buffer, SEND_DATA);
				if (loop_cnt % 500 == 0) {
					snprintf((char *)send_command_data, SEND_COMMAND_DATA, "%d, ", loop_cnt);
					HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
				}
			}
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			strncpy((char *)send_command_data, "(i2c) WRITE FIN -- ALL ZERO", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case I2C_WRITE_ALL_A5:
			HAL_UART_Transmit(&huart2, (uint8_t *)"(i2c) ", 7, 100);
			memset(send_buffer, 0xa5, SEND_DATA);
			for(loop_cnt = 0; loop_cnt < (512 * 1024 / 128); loop_cnt++) {
				CAT24C512_Write(&hi2c2, 128 * loop_cnt, send_buffer, SEND_DATA);
				if (loop_cnt % 500 == 0) {
					snprintf((char *)send_command_data, SEND_COMMAND_DATA, "%d, ", loop_cnt);
					HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
				}
			}
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			strncpy((char *)send_command_data, "(i2c) WRITE FIN -- ALL ZERO", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		default:
			/*
			 * 未定義モード
			 */
			break;
	}
}

static void SPI_ProcessModeCommand(const char *command) {
	long mode = 0;
	char *end_pointer = NULL;
	uint8_t send_command_data[SEND_COMMAND_DATA] = "";
	uint8_t temp[SEND_COMMAND_DATA] = "";
	HAL_StatusTypeDef status = HAL_ERROR;

	uint8_t send_buffer[SEND_DATA] = "";
	uint8_t recv_buffer[RECV_DATA] = "";
	uint16_t loop_cnt = 0, page_loop_cnt = 0;

	uint8_t jedec_id[3] = {0};
	static volatile uint32_t flash_test_result = 0;

//	uint8_t write_data[] = {
//	    0x10, 0x20, 0x30, 0x40,
//	    0x55, 0xAA, 0x12, 0x34,
//	    'S', 'T', 'M', '3', '2'
//	};
	uint8_t write_data[] = "SPI TEST\r\n";

	uint8_t read_data[sizeof(write_data)] = {0};
	mode = strtol(&command[9], &end_pointer, 10);

	/*
	 * 数字以外が含まれていないことを確認する。
	 */
	if (*end_pointer != '\0') {
		return;
	}

	switch ((spi_command_mode)mode) {
		case SPI_ERROR_MODE:
			strncpy((char *)send_command_data, "(spi)MODE : ERROR", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case SPI_READ_JEDEC_ID:
			status = W25Q64JV_ReadJedecId(jedec_id);
			if (status != HAL_OK) {
				snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(spi)JEDEC_ID : ERROR!\r\n");
				HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			} else {
				snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(spi)JEDEC_ID : %2x %2x %2x\r\n", jedec_id[0], jedec_id[1], jedec_id[2]);
				HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			}
			break;

		case SPI_READ_WRITE_TEST:
			status = W25Q64JV_Init(jedec_id);
			if (status != HAL_OK) {
				flash_test_result = 1U;
				return;
			}
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(spi)JEDEC_ID : %2x %2x %2x\r\n", jedec_id[0], jedec_id[1], jedec_id[2]);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);

			status = W25Q64JV_EraseSector(W25Q_TEST_ADDRESS);
			if (status != HAL_OK) {
				flash_test_result = 2U;
				return;
			}
			status = W25Q64JV_Write(W25Q_TEST_ADDRESS, write_data, sizeof(write_data));
			if (status != HAL_OK) {
				flash_test_result = 3U;
				return;
			}
			memset(read_data, 0, sizeof(read_data));
			status = W25Q64JV_Read(W25Q_TEST_ADDRESS, read_data, sizeof(read_data));
	    if (status != HAL_OK) {
	    	flash_test_result = 4U;
	    	return;
	    }
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(spi)WRITE DATA : %s\r\n", write_data);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(spi)READ DATA  : %s\r\n", read_data);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);

			if (memcmp(write_data, read_data, sizeof(write_data)) != 0) {
	    	flash_test_result = 5U;
	    	return;
	    }

	    /*
	     * 成功
	     */
	    flash_test_result = 0x12345678UL;

			break;

		default:
			/*
			 * 未定義モード
			 */
			break;
	}
}

static void QSPI_ProcessModeCommand(const char *command) {
	long mode = 0;
	char *end_pointer = NULL;
	uint8_t send_command_data[SEND_COMMAND_DATA] = "";
	uint8_t temp[SEND_COMMAND_DATA] = "";
	HAL_StatusTypeDef status = HAL_ERROR;

	uint8_t send_buffer[SEND_DATA] = "";
	uint8_t recv_buffer[RECV_DATA] = "";
	uint16_t loop_cnt = 0, page_loop_cnt = 0;

	uint8_t jedec_id[3] = {0};
	static volatile uint32_t flash_test_result = 0;

//	uint8_t write_data[] = {
//	    0x10, 0x20, 0x30, 0x40,
//	    0x55, 0xAA, 0x12, 0x34,
//	    'S', 'T', 'M', '3', '2'
//	};
	uint8_t write_data[] = "QSPI TEST\r\n";

	uint8_t read_data[sizeof(write_data)] = {0};
	mode = strtol(&command[10], &end_pointer, 10);

	/*
	 * 数字以外が含まれていないことを確認する。
	 */
	if (*end_pointer != '\0') {
		return;
	}

	switch ((qspi_command_mode)mode) {
		case QSPI_ERROR_MODE:
			strncpy((char *)send_command_data, "(qspi)MODE : ERROR", SEND_COMMAND_DATA);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 3, 100);
			break;

		case QSPI_READ_JEDEC_ID:
			status = W25Q64JV_ReadJedecId(jedec_id);
			if (status != HAL_OK) {
				snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(qspi)JEDEC_ID : ERROR!\r\n");
				HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			} else {
				snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(qspi)JEDEC_ID : %2x %2x %2x\r\n", jedec_id[0], jedec_id[1], jedec_id[2]);
				HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			}
			break;

		case QSPI_READ_WRITE_TEST:
			status = W25Q64JV_Init(jedec_id);
			if (status != HAL_OK) {
				flash_test_result = 1U;
				return;
			}
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(qspi)JEDEC_ID : %2x %2x %2x\r\n", jedec_id[0], jedec_id[1], jedec_id[2]);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);

			status = W25Q64JV_EraseSector(W25Q_TEST_ADDRESS);
			if (status != HAL_OK) {
				flash_test_result = 2U;
				return;
			}
			status = W25Q64JV_Write(W25Q_TEST_ADDRESS, write_data, sizeof(write_data));
			if (status != HAL_OK) {
				flash_test_result = 3U;
				return;
			}
			memset(read_data, 0, sizeof(read_data));
			status = W25Q64JV_Read(W25Q_TEST_ADDRESS, read_data, sizeof(read_data));
	    if (status != HAL_OK) {
	    	flash_test_result = 4U;
	    	return;
	    }
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(qspi)WRITE DATA : %s\r\n", write_data);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);
			snprintf((char *)send_command_data, SEND_COMMAND_DATA, "(qspi)READ DATA  : %s\r\n", read_data);
			HAL_UART_Transmit(&huart2, send_command_data, SEND_COMMAND_DATA, 100);

			if (memcmp(write_data, read_data, sizeof(write_data)) != 0) {
	    	flash_test_result = 5U;
	    	return;
	    }

	    /*
	     * 成功
	     */
	    flash_test_result = 0x12345678UL;

			break;

		default:
			/*
			 * 未定義モード
			 */
			break;
	}
}

void PacketParser_Init(void) {
	memset(&packet_parser, 0, sizeof(packet_parser));
	packet_parser.state = RX_STATE_COMMAND;
}

void PacketParser_Execute(void) {
	uint8_t received_byte;

	while (UART_Receive_GetByte(&received_byte)) {
		switch (packet_parser.state) {
			case RX_STATE_COMMAND:
				packet_parser.command = received_byte;
				packet_parser.state = RX_STATE_LENGTH;
				break;

			case RX_STATE_LENGTH:
				packet_parser.length = received_byte;
				packet_parser.data_index = 0U;

				if (packet_parser.length == 0U) {
					PacketExecute(packet_parser.command, packet_parser.data, 0U);
					packet_parser.state = RX_STATE_COMMAND;
				}
				else if (packet_parser.length <= PACKET_DATA_SIZE) {
					packet_parser.state = RX_STATE_DATA;
				} else {
					/*
					 * 不正なサイズ
					 */
					packet_parser.state = RX_STATE_COMMAND;
				}
				break;

			case RX_STATE_DATA:
				packet_parser.data[packet_parser.data_index] = received_byte;
				packet_parser.data_index++;

				if (packet_parser.data_index >= packet_parser.length) {
					PacketExecute(packet_parser.command, packet_parser.data, packet_parser.length);
					packet_parser.state = RX_STATE_COMMAND;
				}
				break;

			default:
				packet_parser.state = RX_STATE_COMMAND;
				break;
		}
	}
}

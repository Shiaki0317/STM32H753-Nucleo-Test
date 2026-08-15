/*
 * command_parser.h
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#ifndef INC_COMMAND_PARSER_H_
#define INC_COMMAND_PARSER_H_

#include <stdint.h>
#include "user_util.h"

#define PACKET_DATA_SIZE	128U
#define SEND_COMMAND_DATA	1024U
#define SEND_DATA	128U
#define RECV_DATA	128U

typedef enum {
	RX_STATE_COMMAND,
	RX_STATE_LENGTH,
	RX_STATE_DATA
} RxState;

typedef struct {
	RxState state;

	uint8_t command;
	uint8_t length;

	uint8_t data[PACKET_DATA_SIZE];
	uint8_t data_index;
} PacketParser;

extern I2C_HandleTypeDef hi2c2;
extern UART_HandleTypeDef huart2;

typedef enum {
	I2C_ERROR_MODE = -1,
	I2C_NORMAL_MODE = 0,
	I2C_CHECK_MODE,
	I2C_READ_WRITE_TEST,
	I2C_READ_HEAD,
	I2C_READ_ALL,
	I2C_WRITE_ALL_ZERO,
	I2C_WRITE_ALL_ONE,
	I2C_WRITE_ALL_A5,
	I2C_WRITE_ALL_5A,
	I2C_MAX_MODE,
} i2c_command_mode;

typedef enum {
	SPI_ERROR_MODE = -1,
	SPI_NORMAL_MODE = 0,
	SPI_READ_JEDEC_ID,
	SPI_READ_WRITE_TEST,
	SPI_MAX_MODE,
} spi_command_mode;

typedef enum {
	QSPI_ERROR_MODE = -1,
	QSPI_NORMAL_MODE = 0,
	QSPI_READ_JEDEC_ID,
	QSPI_READ_WRITE_TEST,
	QSPI_MAX_MODE,
} qspi_command_mode;

void CommandParser_Init(void);
void CommandParser_Execute(void);

void PacketExecute(uint8_t command, const uint8_t *data, uint8_t length);

#endif /* INC_COMMAND_PARSER_H_ */

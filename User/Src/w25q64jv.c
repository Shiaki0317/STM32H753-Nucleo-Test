/*
 * w25q64jv.c
 *
 *  Created on: 2026/07/21
 *      Author: shidaa
 */

#include "w25q64jv.h"

/*
 * CubeMXが生成したSPIハンドルです。
 * SPI2を使用する場合はhspi2に変更してください。
 */
extern SPI_HandleTypeDef hspi1;

#define W25Q_SPI_HANDLE			(&hspi1)
#define W25Q_CS_GPIO_Port		(GPIOA)
#define W25Q_CS_Pin					(GPIO_PIN_4)

#define W25Q_CS_LOW() \
    HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET)

#define W25Q_CS_HIGH() \
    HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET)

/* W25Q64JVコマンド */
#define W25Q_CMD_WRITE_ENABLE             0x06U
#define W25Q_CMD_READ_STATUS1             0x05U
#define W25Q_CMD_READ_JEDEC_ID            0x9FU

#define W25Q_CMD_READ_DATA           			0x03U
#define W25Q_CMD_PAGE_PROGRAM        			0x02U
#define W25Q_CMD_SECTOR_ERASE        			0x20U

#define W25Q_CMD_ENABLE_RESET             0x66U
#define W25Q_CMD_RESET                    0x99U

/* Status Register-1 */
#define W25Q_SR1_BUSY                     0x01U
#define W25Q_SR1_WEL                      0x02U

#define W25Q_SPI_TIMEOUT_MS               1000U
#define W25Q_PAGE_PROGRAM_TIMEOUT_MS      100U
#define W25Q_SECTOR_ERASE_TIMEOUT_MS      1000U

static HAL_StatusTypeDef W25Q_SendSimpleCommand(uint8_t command) {
	HAL_StatusTypeDef status;

	W25Q_CS_LOW();
	status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, &command, 1U, W25Q_SPI_TIMEOUT_MS);
	W25Q_CS_HIGH();
	return status;
}

static HAL_StatusTypeDef W25Q_WriteEnable(void) {
	HAL_StatusTypeDef status;
	uint8_t status_register;

	status = W25Q_SendSimpleCommand(W25Q_CMD_WRITE_ENABLE);
	if (status != HAL_OK) {
		return status;
	}

	status = W25Q64JV_ReadStatus1(&status_register);
	if (status != HAL_OK) {
		return status;
	}

	/*
	 * Write Enable Latchがセットされたことを確認します。
	 */
	if ((status_register & W25Q_SR1_WEL) == 0U) {
		return HAL_ERROR;
	}

	return HAL_OK;
}

HAL_StatusTypeDef W25Q64JV_ReadStatus1(uint8_t *status_register) {
	HAL_StatusTypeDef status;
	uint8_t command = W25Q_CMD_READ_STATUS1;

	if (status_register == NULL) {
		return HAL_ERROR;
	}

	W25Q_CS_LOW();

	status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, &command, 1U, W25Q_SPI_TIMEOUT_MS);

	if (status == HAL_OK) {
		status = HAL_SPI_Receive(W25Q_SPI_HANDLE, status_register, 1U, W25Q_SPI_TIMEOUT_MS);
	}

	W25Q_CS_HIGH();

	return status;
}

HAL_StatusTypeDef W25Q64JV_WaitReady(uint32_t timeout_ms) {
	HAL_StatusTypeDef status;
	uint8_t status_register;
	uint32_t start_tick;

	start_tick = HAL_GetTick();

	while (1) {
		status = W25Q64JV_ReadStatus1(&status_register);
		if (status != HAL_OK) {
			return status;
		}

		if ((status_register & W25Q_SR1_BUSY) == 0U) {
			return HAL_OK;
		}

		if ((HAL_GetTick() - start_tick) >= timeout_ms) {
			return HAL_TIMEOUT;
		}

		HAL_Delay(1U);
	}
}

HAL_StatusTypeDef W25Q64JV_Reset(void) {
	HAL_StatusTypeDef status;

	status = W25Q_SendSimpleCommand(W25Q_CMD_ENABLE_RESET);
	if (status != HAL_OK) {
		return status;
	}

	status = W25Q_SendSimpleCommand(W25Q_CMD_RESET);
	if (status != HAL_OK) {
		return status;
	}

	/*
	 * データシート上のリセット時間より十分長く待ちます。
	 */
	HAL_Delay(1U);

	return W25Q64JV_WaitReady(100U);
}

HAL_StatusTypeDef W25Q64JV_ReadJedecId(uint8_t jedec_id[3]) {
	HAL_StatusTypeDef status;
	uint8_t command = W25Q_CMD_READ_JEDEC_ID;

	if (jedec_id == NULL) {
		return HAL_ERROR;
	}

	W25Q_CS_LOW();

	status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, &command, 1U, W25Q_SPI_TIMEOUT_MS);

	if (status == HAL_OK) {
		status = HAL_SPI_Receive(W25Q_SPI_HANDLE, jedec_id, 3U, W25Q_SPI_TIMEOUT_MS);
	}

	W25Q_CS_HIGH();

	return status;
}

HAL_StatusTypeDef W25Q64JV_Init(uint8_t jedec_id[3]) {
	HAL_StatusTypeDef status;

	if (jedec_id == NULL) {
		return HAL_ERROR;
	}

	/*
	 * MCU起動直後は必ずCSをHighにします。
	 */
	W25Q_CS_HIGH();

	/*
	 * Flashの電源立ち上がりを待ちます。
	 */
	HAL_Delay(5U);

	status = W25Q64JV_Reset();
	if (status != HAL_OK) {
		return status;
	}

	status = W25Q64JV_ReadJedecId(jedec_id);
	if (status != HAL_OK) {
		return status;
	}

	/*
	 * Winbond Manufacturer ID = 0xEF
	 * 64Mbit Capacity ID      = 0x17
	 *
	 * Memory Typeは品種によって0x40または0x70などがあり得るため、
	 * ここではManufacturerとCapacityを確認します。
	 */
	if ((jedec_id[0] != 0xEFU) || (jedec_id[2] != 0x17U)) {
		return HAL_ERROR;
	}

	return HAL_OK;
}

HAL_StatusTypeDef W25Q64JV_Read(uint32_t address, uint8_t *data, uint32_t length) {
	HAL_StatusTypeDef status;
	uint8_t tx_header[4];
	uint8_t rx_header[4];

	/*
	 * データ読出し用のダミー送信バッファ。
	 * SPIでは受信するためにもMOSIからデータを送信して
	 * クロックを生成する必要があります。
	 */
	uint8_t dummy_tx[64];

	if ((data == NULL) && (length != 0U)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	if ((address >= W25Q64JV_SIZE_BYTES) || (length > (W25Q64JV_SIZE_BYTES - address))) {
		return HAL_ERROR;
	}

	status = W25Q64JV_WaitReady(1000U);
	if (status != HAL_OK) {
		return status;
	}

	tx_header[0] = W25Q_CMD_READ_DATA; /* 0x03 */
	tx_header[1] = (uint8_t)(address >> 16);
	tx_header[2] = (uint8_t)(address >> 8);
	tx_header[3] = (uint8_t)(address);

	memset(dummy_tx, 0xFF, sizeof(dummy_tx));
	memset(rx_header, 0, sizeof(rx_header));

	W25Q_CS_LOW();

	/*
	 * HAL_SPI_Transmit()ではなくTransmitReceive()を使用します。
	 *
	 * コマンド／アドレス送信中にMISOから入力される不要なデータを
	 * rx_headerへ読み捨てることで、RX FIFOを空に保ちます。
	 */
	status = HAL_SPI_TransmitReceive(W25Q_SPI_HANDLE, tx_header, rx_header, sizeof(tx_header), W25Q_SPI_TIMEOUT_MS);

	while ((status == HAL_OK) && (length > 0U)) {
		uint16_t chunk;

		if (length > sizeof(dummy_tx)) {
			chunk = sizeof(dummy_tx);
		} else {
			chunk = (uint16_t)length;
		}

		/*
		 * 0xFFをMOSIへ送信してクロックを発生させ、
		 * MISOからFlashデータを受信します。
		 */
		status = HAL_SPI_TransmitReceive(W25Q_SPI_HANDLE, dummy_tx, data, chunk, W25Q_SPI_TIMEOUT_MS);

		data += chunk;
		length -= chunk;
	}

	W25Q_CS_HIGH();

	return status;
}

HAL_StatusTypeDef W25Q64JV_ProgramPage(uint32_t address, const uint8_t *data, uint32_t length) {
	HAL_StatusTypeDef status;
	uint8_t header[4];
	uint32_t page_offset;

	if ((data == NULL) || (length == 0U)) {
		return HAL_ERROR;
	}

	if (length > W25Q64JV_PAGE_SIZE) {
		return HAL_ERROR;
	}

	if ((address >= W25Q64JV_SIZE_BYTES) || (length > (W25Q64JV_SIZE_BYTES - address))) {
		return HAL_ERROR;
	}

	page_offset = address & (W25Q64JV_PAGE_SIZE - 1U);

	/*
	 * ページ境界をまたぐ書込みは禁止します。
	 */
	if ((page_offset + length) > W25Q64JV_PAGE_SIZE) {
		return HAL_ERROR;
	}

	status = W25Q64JV_WaitReady(1000U);
	if (status != HAL_OK) {
		return status;
	}

	status = W25Q_WriteEnable();
	if (status != HAL_OK) {
		return status;
	}

	header[0] = W25Q_CMD_PAGE_PROGRAM;
	header[1] = (uint8_t)(address >> 16);
	header[2] = (uint8_t)(address >> 8);
	header[3] = (uint8_t)(address);

	W25Q_CS_LOW();

	status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, header, sizeof(header), W25Q_SPI_TIMEOUT_MS);

	if (status == HAL_OK) {
		status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, (uint8_t *)data, (uint16_t)length, W25Q_SPI_TIMEOUT_MS);
	}

	W25Q_CS_HIGH();

	if (status != HAL_OK) {
		return status;
	}

	return W25Q64JV_WaitReady(W25Q_PAGE_PROGRAM_TIMEOUT_MS);
}

HAL_StatusTypeDef W25Q64JV_Write(uint32_t address, const uint8_t *data, uint32_t length) {
	HAL_StatusTypeDef status;

	if ((data == NULL) && (length != 0U)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	if ((address >= W25Q64JV_SIZE_BYTES) || (length > (W25Q64JV_SIZE_BYTES - address))) {
		return HAL_ERROR;
	}

	while (length > 0U) {
		uint32_t page_offset;
		uint32_t page_remaining;
		uint32_t write_length;

		page_offset = address & (W25Q64JV_PAGE_SIZE - 1U);

		page_remaining = W25Q64JV_PAGE_SIZE - page_offset;

		write_length = (length < page_remaining) ? length : page_remaining;

		status = W25Q64JV_ProgramPage(address, data, write_length);

		if (status != HAL_OK) {
			return status;
		}

		address += write_length;
		data += write_length;
		length -= write_length;
	}

	return HAL_OK;
}

HAL_StatusTypeDef W25Q64JV_EraseSector(uint32_t address) {
	HAL_StatusTypeDef status;
	uint8_t command[4];

	if (address >= W25Q64JV_SIZE_BYTES) {
		return HAL_ERROR;
	}

	/*
	 * 指定アドレスを4KBセクタの先頭に切り下げます。
	 */
	address &= ~(W25Q64JV_SECTOR_SIZE - 1U);

	status = W25Q64JV_WaitReady(1000U);
	if (status != HAL_OK) {
		return status;
	}

	status = W25Q_WriteEnable();
	if (status != HAL_OK) {
		return status;
	}

	command[0] = W25Q_CMD_SECTOR_ERASE;
	command[1] = (uint8_t)(address >> 16);
	command[2] = (uint8_t)(address >> 8);
	command[3] = (uint8_t)(address);

	W25Q_CS_LOW();

	status = HAL_SPI_Transmit(W25Q_SPI_HANDLE, command, sizeof(command), W25Q_SPI_TIMEOUT_MS);

	W25Q_CS_HIGH();

	if (status != HAL_OK) {
		return status;
	}

	return W25Q64JV_WaitReady(W25Q_SECTOR_ERASE_TIMEOUT_MS);
}

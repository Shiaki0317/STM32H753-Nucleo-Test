#include "w25q64jv_qspi.h"

#include <string.h>

/* Commands */
#define W25Q_CMD_WRITE_ENABLE            0x06U
#define W25Q_CMD_READ_SR1                0x05U
#define W25Q_CMD_READ_SR2                0x35U
#define W25Q_CMD_WRITE_SR2               0x31U
#define W25Q_CMD_JEDEC_ID                0x9FU

#define W25Q_CMD_QUAD_READ               0x6BU
#define W25Q_CMD_QUAD_PROGRAM            0x32U
#define W25Q_CMD_SECTOR_ERASE            0x20U

#define W25Q_CMD_ENABLE_RESET            0x66U
#define W25Q_CMD_RESET_DEVICE            0x99U

/* Status bits */
#define W25Q_SR1_BUSY                    0x01U
#define W25Q_SR1_WEL                     0x02U
#define W25Q_SR2_QE                      0x02U

/* Timeouts */
#define W25Q_COMMAND_TIMEOUT_MS          100U
#define W25Q_READ_TIMEOUT_MS             5000U
#define W25Q_REGISTER_TIMEOUT_MS         1000U
#define W25Q_PAGE_PROGRAM_TIMEOUT_MS     1000U
#define W25Q_SECTOR_ERASE_TIMEOUT_MS     5000U

#define W25Q_QUAD_READ_DUMMY_CYCLES      8U

static void W25Q_Quad_CommandInit(QSPI_CommandTypeDef *command) {
	memset(command, 0, sizeof(*command));

	command->InstructionMode   = QSPI_INSTRUCTION_1_LINE;
	command->AddressMode       = QSPI_ADDRESS_NONE;
	command->AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
	command->DataMode          = QSPI_DATA_NONE;
	command->DummyCycles       = 0U;
	command->DdrMode           = QSPI_DDR_MODE_DISABLE;
	command->DdrHoldHalfCycle  = QSPI_DDR_HHC_ANALOG_DELAY;
	command->SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
}

static bool W25Q_Quad_IsRangeValid(uint32_t address, size_t length) {
	if (length == 0U) {
		return address <= W25Q64JV_FLASH_SIZE_BYTES;
	}

	if (address >= W25Q64JV_FLASH_SIZE_BYTES) {
		return false;
	}

	return length <= ((size_t)W25Q64JV_FLASH_SIZE_BYTES - (size_t)address);
}

static HAL_StatusTypeDef W25Q_Quad_SendInstruction(QSPI_HandleTypeDef *hqspi, uint8_t instruction) {
	QSPI_CommandTypeDef command;

	if (hqspi == NULL) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = instruction;

	return HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS);
}

static HAL_StatusTypeDef W25Q_Quad_ReadRegister(QSPI_HandleTypeDef *hqspi, uint8_t instruction, uint8_t *value) {
	QSPI_CommandTypeDef command;

	if ((hqspi == NULL) || (value == NULL)) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = instruction;
	command.DataMode     = QSPI_DATA_1_LINE;
	command.NbData       = 1U;

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	return HAL_QSPI_Receive(hqspi, value, W25Q_COMMAND_TIMEOUT_MS);
}

static HAL_StatusTypeDef W25Q_Quad_WaitStatus(QSPI_HandleTypeDef *hqspi, uint8_t match, uint8_t mask, uint32_t timeout_ms) {
	QSPI_CommandTypeDef command;
	QSPI_AutoPollingTypeDef polling;

	if (hqspi == NULL) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	memset(&polling, 0, sizeof(polling));

	command.Instruction = W25Q_CMD_READ_SR1;
	command.DataMode     = QSPI_DATA_1_LINE;
	command.NbData       = 1U;

	polling.Match           = match;
	polling.Mask            = mask;
	polling.MatchMode       = QSPI_MATCH_MODE_AND;
	polling.StatusBytesSize = 1U;
	polling.Interval        = 0x10U;
	polling.AutomaticStop   = QSPI_AUTOMATIC_STOP_ENABLE;

	return HAL_QSPI_AutoPolling(hqspi, &command, &polling, timeout_ms);
}

static HAL_StatusTypeDef W25Q_Quad_WaitReady(QSPI_HandleTypeDef *hqspi, uint32_t timeout_ms) {
	return W25Q_Quad_WaitStatus(hqspi, 0x00U, W25Q_SR1_BUSY, timeout_ms);
}

static HAL_StatusTypeDef W25Q_Quad_WriteEnable(QSPI_HandleTypeDef *hqspi) {
	if (W25Q_Quad_SendInstruction(hqspi, W25Q_CMD_WRITE_ENABLE) != HAL_OK) {
		return HAL_ERROR;
	}

	return W25Q_Quad_WaitStatus(hqspi, W25Q_SR1_WEL, W25Q_SR1_WEL, W25Q_REGISTER_TIMEOUT_MS);
}

static HAL_StatusTypeDef W25Q_Quad_EnableQuadMode(QSPI_HandleTypeDef *hqspi) {
	QSPI_CommandTypeDef command;
	uint8_t status2;

	if (W25Q64JV_Quad_ReadStatus2(hqspi, &status2) != HAL_OK) {
		return HAL_ERROR;
	}

	/*
	 * W25Q64JV-IQ has QE fixed to 1.
	 * W25Q64JV-IM allows QE to be programmed.
	 */
	if ((status2 & W25Q_SR2_QE) != 0U) {
		return HAL_OK;
	}

	if (W25Q_Quad_WriteEnable(hqspi) != HAL_OK) {
		return HAL_ERROR;
	}

	status2 |= W25Q_SR2_QE;

	W25Q_Quad_CommandInit(&command);
	command.Instruction = W25Q_CMD_WRITE_SR2;
	command.DataMode     = QSPI_DATA_1_LINE;
	command.NbData       = 1U;

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	if (HAL_QSPI_Transmit(hqspi, &status2, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	if (W25Q_Quad_WaitReady(hqspi, W25Q_REGISTER_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	if (W25Q64JV_Quad_ReadStatus2(hqspi, &status2) != HAL_OK) {
		return HAL_ERROR;
	}

	return ((status2 & W25Q_SR2_QE) != 0U) ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef W25Q_Quad_PageProgram(QSPI_HandleTypeDef *hqspi, uint32_t address, const uint8_t *data, size_t length) {
	QSPI_CommandTypeDef command;
	uint32_t page_offset;

	if ((hqspi == NULL) || (data == NULL) || (length == 0U) || (length > W25Q64JV_PAGE_SIZE_BYTES) || !W25Q_Quad_IsRangeValid(address, length)) {
		return HAL_ERROR;
	}

	page_offset = address & (W25Q64JV_PAGE_SIZE_BYTES - 1UL);
	if ((page_offset + length) > W25Q64JV_PAGE_SIZE_BYTES) {
		return HAL_ERROR;
	}

	if (W25Q_Quad_WriteEnable(hqspi) != HAL_OK) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = W25Q_CMD_QUAD_PROGRAM;
	command.AddressMode = QSPI_ADDRESS_1_LINE;
	command.AddressSize = QSPI_ADDRESS_24_BITS;
	command.Address     = address;
	command.DataMode    = QSPI_DATA_4_LINES;
	command.NbData      = (uint32_t)length;

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	if (HAL_QSPI_Transmit(hqspi, (uint8_t *)(uintptr_t)data, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	return W25Q_Quad_WaitReady(hqspi, W25Q_PAGE_PROGRAM_TIMEOUT_MS);
}

HAL_StatusTypeDef W25Q64JV_Quad_Init(QSPI_HandleTypeDef *hqspi) {
	W25Q64JV_JedecId id;

	if (hqspi == NULL) {
		return HAL_ERROR;
	}

	if (W25Q64JV_Quad_Reset(hqspi) != HAL_OK) {
		return HAL_ERROR;
	}

	HAL_Delay(1U);

	if (W25Q64JV_Quad_ReadJedecId(hqspi, &id) != HAL_OK) {
		return HAL_ERROR;
	}

	if (!W25Q64JV_Quad_IsSupportedId(&id)) {
		return HAL_ERROR;
	}

	return W25Q_Quad_EnableQuadMode(hqspi);
}

HAL_StatusTypeDef W25Q64JV_Quad_Reset(QSPI_HandleTypeDef *hqspi) {
	if (W25Q_Quad_SendInstruction(hqspi, W25Q_CMD_ENABLE_RESET) != HAL_OK) {
		return HAL_ERROR;
	}

	return W25Q_Quad_SendInstruction(hqspi, W25Q_CMD_RESET_DEVICE);
}

HAL_StatusTypeDef W25Q64JV_Quad_ReadJedecId(QSPI_HandleTypeDef *hqspi, W25Q64JV_JedecId *id) {
	QSPI_CommandTypeDef command;
	uint8_t raw_id[3];

	if ((hqspi == NULL) || (id == NULL)) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = W25Q_CMD_JEDEC_ID;
	command.DataMode     = QSPI_DATA_1_LINE;
	command.NbData       = sizeof(raw_id);

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	if (HAL_QSPI_Receive(hqspi, raw_id, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	id->manufacturer_id = raw_id[0];
	id->memory_type     = raw_id[1];
	id->capacity_id     = raw_id[2];

	return HAL_OK;
}

bool W25Q64JV_Quad_IsSupportedId(const W25Q64JV_JedecId *id) {
	if (id == NULL) {
		return false;
	}

	return (id->manufacturer_id == 0xEFU) && ((id->memory_type == 0x40U) || (id->memory_type == 0x70U)) && (id->capacity_id == 0x17U);
}

HAL_StatusTypeDef W25Q64JV_Quad_ReadStatus1(QSPI_HandleTypeDef *hqspi, uint8_t *status) {
	return W25Q_Quad_ReadRegister(hqspi, W25Q_CMD_READ_SR1, status);
}

HAL_StatusTypeDef W25Q64JV_Quad_ReadStatus2(QSPI_HandleTypeDef *hqspi, uint8_t *status) {
	return W25Q_Quad_ReadRegister(hqspi, W25Q_CMD_READ_SR2, status);
}

HAL_StatusTypeDef W25Q64JV_Quad_Read(QSPI_HandleTypeDef *hqspi, uint32_t address, uint8_t *data, size_t length) {
	QSPI_CommandTypeDef command;

	if ((hqspi == NULL) || ((data == NULL) && (length != 0U)) || !W25Q_Quad_IsRangeValid(address, length)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = W25Q_CMD_QUAD_READ;
	command.AddressMode = QSPI_ADDRESS_1_LINE;
	command.AddressSize = QSPI_ADDRESS_24_BITS;
	command.Address     = address;
	command.DataMode    = QSPI_DATA_4_LINES;
	command.DummyCycles = W25Q_QUAD_READ_DUMMY_CYCLES;
	command.NbData      = (uint32_t)length;

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	return HAL_QSPI_Receive(hqspi, data, W25Q_READ_TIMEOUT_MS);
}

HAL_StatusTypeDef W25Q64JV_Quad_Program(QSPI_HandleTypeDef *hqspi, uint32_t address, const uint8_t *data, size_t length) {
	size_t page_remaining;
	size_t chunk;

	if ((hqspi == NULL) || ((data == NULL) && (length != 0U)) || !W25Q_Quad_IsRangeValid(address, length)) {
		return HAL_ERROR;
	}

	while (length > 0U) {
		page_remaining = W25Q64JV_PAGE_SIZE_BYTES - (address & (W25Q64JV_PAGE_SIZE_BYTES - 1UL));

		chunk = (length < page_remaining) ? length : page_remaining;
		if (W25Q_Quad_PageProgram(hqspi, address, data, chunk) != HAL_OK) {
			return HAL_ERROR;
		}

		address += (uint32_t)chunk;
		data    += chunk;
		length  -= chunk;
	}

	return HAL_OK;
}

HAL_StatusTypeDef W25Q64JV_Quad_EraseSector4K(QSPI_HandleTypeDef *hqspi, uint32_t address) {
	QSPI_CommandTypeDef command;

	if ((hqspi == NULL) || (address >= W25Q64JV_FLASH_SIZE_BYTES) || ((address & (W25Q64JV_SECTOR_SIZE_BYTES - 1UL)) != 0U)) {
		return HAL_ERROR;
	}

	if (W25Q_Quad_WriteEnable(hqspi) != HAL_OK) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	command.Instruction = W25Q_CMD_SECTOR_ERASE;
	command.AddressMode = QSPI_ADDRESS_1_LINE;
	command.AddressSize = QSPI_ADDRESS_24_BITS;
	command.Address     = address;

	if (HAL_QSPI_Command(hqspi, &command, W25Q_COMMAND_TIMEOUT_MS) != HAL_OK) {
		return HAL_ERROR;
	}

	return W25Q_Quad_WaitReady(hqspi, W25Q_SECTOR_ERASE_TIMEOUT_MS);
}

HAL_StatusTypeDef W25Q64JV_Quad_EraseRange4K(QSPI_HandleTypeDef *hqspi, uint32_t address, size_t length) {
	uint32_t first_sector;
	uint32_t last_sector;
	uint32_t sector;

	if ((hqspi == NULL) || !W25Q_Quad_IsRangeValid(address, length)) {
		return HAL_ERROR;
	}

	if (length == 0U) {
		return HAL_OK;
	}

	first_sector = address & ~(W25Q64JV_SECTOR_SIZE_BYTES - 1UL);

	last_sector = (uint32_t)(((size_t)address + length - 1U) & ~(size_t)(W25Q64JV_SECTOR_SIZE_BYTES - 1UL));

	sector = first_sector;
	for (;;) {
		if (W25Q64JV_Quad_EraseSector4K(hqspi, sector) != HAL_OK) {
			return HAL_ERROR;
		}

		if (sector == last_sector) {
			break;
		}

		sector += W25Q64JV_SECTOR_SIZE_BYTES;
	}

	return HAL_OK;
}

HAL_StatusTypeDef W25Q64JV_Quad_EnableMemoryMapped(QSPI_HandleTypeDef *hqspi) {
	QSPI_CommandTypeDef command;
	QSPI_MemoryMappedTypeDef memory_mapped;

	if (hqspi == NULL) {
		return HAL_ERROR;
	}

	W25Q_Quad_CommandInit(&command);
	memset(&memory_mapped, 0, sizeof(memory_mapped));

	command.Instruction = W25Q_CMD_QUAD_READ;
	command.AddressMode = QSPI_ADDRESS_1_LINE;
	command.AddressSize = QSPI_ADDRESS_24_BITS;
	command.DataMode    = QSPI_DATA_4_LINES;
	command.DummyCycles = W25Q_QUAD_READ_DUMMY_CYCLES;

	memory_mapped.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
	memory_mapped.TimeOutPeriod = 0U;

	return HAL_QSPI_MemoryMapped(hqspi, &command, &memory_mapped);
}

HAL_StatusTypeDef W25Q64JV_Quad_DisableMemoryMapped(QSPI_HandleTypeDef *hqspi) {
	if (hqspi == NULL) {
		return HAL_ERROR;
	}

	return HAL_QSPI_Abort(hqspi);
}

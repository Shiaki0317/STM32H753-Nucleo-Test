/*
 * uart_receive.h
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#ifndef INC_UART_RECEIVE_H_
#define INC_UART_RECEIVE_H_

#include "user_util.h"

#define UART_RX_BUFFER_SIZE 256U

void Error_Handler(void);
void UART_Receive_Start(void);
bool UART_Receive_GetByte(uint8_t *data);
uint16_t UART_Receive_GetCount(void);
bool UART_Receive_IsOverflow(void);
void UART_Receive_ClearOverflow(void);

#endif /* INC_UART_RECEIVE_H_ */

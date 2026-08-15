/*
 * uart_receive.c
 *
 *  Created on: 2026/07/20
 *      Author: shidaa
 */

#include "uart_receive.h"

extern UART_HandleTypeDef huart2;

/*
 * UART_RX_BUFFER_SIZEは2のべき乗にする。
 * 例：64, 128, 256, 512
 */
static uint8_t rx_buffer[UART_RX_BUFFER_SIZE];

/*
 * head：割り込み側が書き込む位置
 * tail：mainループ側が読み出す位置
 */
static volatile uint16_t rx_head = 0U;
static volatile uint16_t rx_tail = 0U;

static uint8_t rx_byte;
static volatile bool rx_overflow = false;

/**
 * @brief UARTの1文字受信を開始する
 */
void UART_Receive_Start(void)
{
    rx_head = 0U;
    rx_tail = 0U;
    rx_overflow = false;

    if (HAL_UART_Receive_IT(&huart2, &rx_byte, 1U) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief リングバッファから1文字取り出す
 *
 * @return true  文字を取得した
 * @return false バッファが空
 */
bool UART_Receive_GetByte(uint8_t *data)
{
    if (data == NULL) {
        return false;
    }

    if (rx_head == rx_tail) {
        return false;
    }

    *data = rx_buffer[rx_tail];

    rx_tail = (rx_tail + 1U) & (UART_RX_BUFFER_SIZE - 1U);

    return true;
}

/**
 * @brief 現在格納されている文字数を返す
 */
uint16_t UART_Receive_GetCount(void)
{
    return (rx_head - rx_tail) & (UART_RX_BUFFER_SIZE - 1U);
}

bool UART_Receive_IsOverflow(void)
{
    return rx_overflow;
}

void UART_Receive_ClearOverflow(void)
{
    rx_overflow = false;
}

/**
 * @brief HALによるUART受信完了コールバック
 *
 * HAL_UART_Receive_IT()で1バイトを指定しているため、
 * 1文字受信するごとに呼ばれる。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uint16_t next_head;

        next_head =
            (rx_head + 1U) & (UART_RX_BUFFER_SIZE - 1U);

        if (next_head != rx_tail) {
            /*
             * 空きがある場合は格納する
             */
            rx_buffer[rx_head] = rx_byte;
            rx_head = next_head;
        } else {
            /*
             * バッファが満杯
             */
            rx_overflow = true;
        }

        /*
         * 受信データを送信する。
         */
        (void)HAL_UART_Transmit(&huart2, &rx_byte, 1U, 100);

        /*
         * 次の1文字を受信するために再登録する。
         */
        (void)HAL_UART_Receive_IT(&huart2, &rx_byte, 1U);
    }
}

/**
 * @brief UARTエラー発生時のコールバック
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        /*
         * オーバーランなどで受信が停止した場合に再開する。
         */
        (void)HAL_UART_Receive_IT(&huart2, &rx_byte, 1U);
    }
}

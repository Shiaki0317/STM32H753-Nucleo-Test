/**
  ******************************************************************************
  * @file    tcp_echo.h
  * @brief   TCP echo server public interface.
  ******************************************************************************
  */

#ifndef TCP_ECHO_H
#define TCP_ECHO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/err.h"

/** Development TLS Echo service port. */
#define TCP_ECHO_PORT 4433U

/**
  * Development-only application authentication token carried inside TLS.
  *
  * Override this macro from the compiler settings for each deployment. Because
  * the protocol is plain TCP, the token is not protected against sniffing.
  */
#ifndef TCP_ECHO_AUTH_TOKEN
#define TCP_ECHO_AUTH_TOKEN "stm32h753"
#endif

/**
  * @brief  Create and start the TCP echo listening endpoint.
  * @retval ERR_OK on success, otherwise an lwIP error code.
  */
err_t TCP_Echo_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* TCP_ECHO_H */

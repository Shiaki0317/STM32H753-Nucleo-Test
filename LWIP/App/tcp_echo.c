/**
  ******************************************************************************
  * @file    tcp_echo.c
  * @brief   Non-blocking TLS Echo server using lwIP Raw API and MbedTLS.
  ******************************************************************************
  */

#include "tcp_echo.h"

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "mbedtls/certs.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <string.h>

#define TCP_ECHO_POLL_INTERVAL          2U
#define TCP_ECHO_BACKLOG                1U
#define TCP_ECHO_TLS_TIMEOUT_POLLS      20U
#define TCP_ECHO_AUTH_TIMEOUT_POLLS     10U
#define TCP_ECHO_AUTH_LINE_MAX_LENGTH   80U
#define TCP_ECHO_TLS_READ_BUFFER_LENGTH 1024U
#define TCP_ECHO_APP_TX_BUFFER_LENGTH   2048U
#define TCP_ECHO_DRIVE_ITERATION_LIMIT  16U
#define TCP_ECHO_AUTH_PREFIX            "AUTH "

typedef enum
{
  TCP_ECHO_AUTH_PENDING = 0,
  TCP_ECHO_AUTHENTICATED,
  TCP_ECHO_AUTH_REJECTED
} TCP_EchoAuthState_t;

typedef struct
{
  struct tcp_pcb *pcb;
  struct pbuf *encrypted_rx;
  mbedtls_ssl_context ssl;
  u8_t app_tx[TCP_ECHO_APP_TX_BUFFER_LENGTH];
  size_t app_tx_length;
  size_t app_tx_offset;
  u8_t auth_line[TCP_ECHO_AUTH_LINE_MAX_LENGTH];
  u8_t auth_line_length;
  u8_t poll_count;
  u8_t tls_established;
  u8_t close_requested;
  TCP_EchoAuthState_t auth_state;
} TCP_EchoConnection_t;

static struct tcp_pcb *TCP_EchoListenPcb;
static mbedtls_entropy_context TCP_EchoEntropy;
static mbedtls_ctr_drbg_context TCP_EchoCtrDrbg;
static mbedtls_ssl_config TCP_EchoSslConfig;
static mbedtls_x509_crt TCP_EchoServerCertificate;
static mbedtls_pk_context TCP_EchoServerKey;
static TCP_EchoConnection_t TCP_EchoConnection;
static u8_t TCP_EchoConnectionActive;

static const char TCP_EchoPersonalization[] = "stm32h753_tls_echo";
static const char TCP_EchoAuthOk[] = "OK\r\n";
static const char TCP_EchoAuthFailed[] = "ERR authentication failed\r\n";
static const char TCP_EchoAuthTimeout[] = "ERR authentication timeout\r\n";

static err_t TCP_Echo_Accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t TCP_Echo_Receive(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err);
static err_t TCP_Echo_Sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t TCP_Echo_Poll(void *arg, struct tcp_pcb *tpcb);
static void TCP_Echo_Error(void *arg, err_t err);
static err_t TCP_Echo_Drive(TCP_EchoConnection_t *connection);
static int TCP_Echo_TlsSend(void *context, const unsigned char *buffer,
                            size_t length);
static int TCP_Echo_TlsReceive(void *context, unsigned char *buffer,
                               size_t length);
static void TCP_Echo_ProcessApplicationData(TCP_EchoConnection_t *connection,
                                            const u8_t *data,
                                            size_t length);
static void TCP_Echo_QueueApplicationReply(TCP_EchoConnection_t *connection,
                                           const char *reply,
                                           size_t length);
static err_t TCP_Echo_Close(TCP_EchoConnection_t *connection);
static err_t TCP_Echo_Abort(TCP_EchoConnection_t *connection);
static void TCP_Echo_FreeConnection(TCP_EchoConnection_t *connection);

err_t TCP_Echo_Init(void)
{
  struct tcp_pcb *pcb;
  struct tcp_pcb *listen_pcb;
  const unsigned char *password;
  size_t password_length;
  err_t lwip_err;
  int tls_err;

  if (TCP_EchoListenPcb != NULL)
  {
    return ERR_ALREADY;
  }

  mbedtls_entropy_init(&TCP_EchoEntropy);
  mbedtls_ctr_drbg_init(&TCP_EchoCtrDrbg);
  mbedtls_ssl_config_init(&TCP_EchoSslConfig);
  mbedtls_x509_crt_init(&TCP_EchoServerCertificate);
  mbedtls_pk_init(&TCP_EchoServerKey);

  tls_err = mbedtls_ctr_drbg_seed(&TCP_EchoCtrDrbg,
                                  mbedtls_entropy_func,
                                  &TCP_EchoEntropy,
                                  (const unsigned char *)TCP_EchoPersonalization,
                                  sizeof(TCP_EchoPersonalization) - 1U);
  if (tls_err != 0)
  {
    return ERR_IF;
  }

  tls_err = mbedtls_x509_crt_parse(
      &TCP_EchoServerCertificate,
      (const unsigned char *)mbedtls_test_srv_crt_ec,
      mbedtls_test_srv_crt_ec_len);
  if (tls_err != 0)
  {
    return ERR_IF;
  }

  password = (const unsigned char *)mbedtls_test_srv_pwd_ec;
  password_length = mbedtls_test_srv_pwd_ec_len;
  tls_err = mbedtls_pk_parse_key(
      &TCP_EchoServerKey,
      (const unsigned char *)mbedtls_test_srv_key_ec,
      mbedtls_test_srv_key_ec_len,
      password,
      password_length);
  if (tls_err != 0)
  {
    return ERR_IF;
  }

  tls_err = mbedtls_ssl_config_defaults(&TCP_EchoSslConfig,
                                        MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
  if (tls_err != 0)
  {
    return ERR_IF;
  }

  mbedtls_ssl_conf_rng(&TCP_EchoSslConfig,
                       mbedtls_ctr_drbg_random,
                       &TCP_EchoCtrDrbg);
  mbedtls_ssl_conf_authmode(&TCP_EchoSslConfig, MBEDTLS_SSL_VERIFY_NONE);
  tls_err = mbedtls_ssl_conf_own_cert(&TCP_EchoSslConfig,
                                     &TCP_EchoServerCertificate,
                                     &TCP_EchoServerKey);
  if (tls_err != 0)
  {
    return ERR_IF;
  }

  pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
  if (pcb == NULL)
  {
    return ERR_MEM;
  }

  lwip_err = tcp_bind(pcb, IP_ADDR_ANY, TCP_ECHO_PORT);
  if (lwip_err != ERR_OK)
  {
    tcp_abort(pcb);
    return lwip_err;
  }

  listen_pcb = tcp_listen_with_backlog_and_err(pcb,
                                                TCP_ECHO_BACKLOG,
                                                &lwip_err);
  if (listen_pcb == NULL)
  {
    tcp_abort(pcb);
    return lwip_err;
  }

  TCP_EchoListenPcb = listen_pcb;
  tcp_accept(TCP_EchoListenPcb, TCP_Echo_Accept);
  return ERR_OK;
}

static err_t TCP_Echo_Accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  TCP_EchoConnection_t *connection;
  int tls_err;

  LWIP_UNUSED_ARG(arg);
  if ((err != ERR_OK) || (newpcb == NULL))
  {
    return ERR_VAL;
  }
  if (TCP_EchoConnectionActive != 0U)
  {
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  connection = &TCP_EchoConnection;
  (void)memset(connection, 0, sizeof(*connection));

  mbedtls_ssl_init(&connection->ssl);
  tls_err = mbedtls_ssl_setup(&connection->ssl, &TCP_EchoSslConfig);
  if (tls_err != 0)
  {
    mbedtls_ssl_free(&connection->ssl);
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  connection->pcb = newpcb;
  TCP_EchoConnectionActive = 1U;
  mbedtls_ssl_set_bio(&connection->ssl,
                      connection,
                      TCP_Echo_TlsSend,
                      TCP_Echo_TlsReceive,
                      NULL);
  tcp_arg(newpcb, connection);
  tcp_recv(newpcb, TCP_Echo_Receive);
  tcp_sent(newpcb, TCP_Echo_Sent);
  tcp_err(newpcb, TCP_Echo_Error);
  tcp_poll(newpcb, TCP_Echo_Poll, TCP_ECHO_POLL_INTERVAL);
  return TCP_Echo_Drive(connection);
}

static err_t TCP_Echo_Receive(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)arg;

  if (connection == NULL)
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    tcp_abort(tpcb);
    return ERR_ABRT;
  }
  if (err != ERR_OK)
  {
    if (p != NULL)
    {
      pbuf_free(p);
    }
    return TCP_Echo_Abort(connection);
  }

  if (p == NULL)
  {
    connection->close_requested = 1U;
  }
  else if (connection->encrypted_rx == NULL)
  {
    connection->encrypted_rx = p;
  }
  else
  {
    pbuf_cat(connection->encrypted_rx, p);
  }
  return TCP_Echo_Drive(connection);
}

static err_t TCP_Echo_Sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)arg;

  LWIP_UNUSED_ARG(tpcb);
  LWIP_UNUSED_ARG(len);
  if (connection == NULL)
  {
    return ERR_ABRT;
  }
  return TCP_Echo_Drive(connection);
}

static err_t TCP_Echo_Poll(void *arg, struct tcp_pcb *tpcb)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)arg;

  LWIP_UNUSED_ARG(tpcb);
  if (connection == NULL)
  {
    return ERR_ABRT;
  }

  connection->poll_count++;
  if (((connection->tls_established == 0U) &&
       (connection->poll_count >= TCP_ECHO_TLS_TIMEOUT_POLLS)) ||
      ((connection->tls_established != 0U) &&
       (connection->auth_state == TCP_ECHO_AUTH_PENDING) &&
       (connection->poll_count >= TCP_ECHO_AUTH_TIMEOUT_POLLS)))
  {
    if (connection->tls_established == 0U)
    {
      return TCP_Echo_Abort(connection);
    }
    connection->auth_state = TCP_ECHO_AUTH_REJECTED;
    connection->close_requested = 1U;
    TCP_Echo_QueueApplicationReply(connection,
                                   TCP_EchoAuthTimeout,
                                   sizeof(TCP_EchoAuthTimeout) - 1U);
  }
  return TCP_Echo_Drive(connection);
}

static void TCP_Echo_Error(void *arg, err_t err)
{
  LWIP_UNUSED_ARG(err);
  TCP_Echo_FreeConnection((TCP_EchoConnection_t *)arg);
}

static err_t TCP_Echo_Drive(TCP_EchoConnection_t *connection)
{
  u8_t application_data[TCP_ECHO_TLS_READ_BUFFER_LENGTH];
  u8_t iteration;
  int tls_result;

  for (iteration = 0U;
       iteration < TCP_ECHO_DRIVE_ITERATION_LIMIT;
       iteration++)
  {
    if (connection->tls_established == 0U)
    {
      tls_result = mbedtls_ssl_handshake(&connection->ssl);
      if (tls_result == 0)
      {
        connection->tls_established = 1U;
        connection->poll_count = 0U;
        continue;
      }
      if ((tls_result == MBEDTLS_ERR_SSL_WANT_READ) ||
          (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE))
      {
        return ERR_OK;
      }
      return TCP_Echo_Abort(connection);
    }

    if (connection->app_tx_offset < connection->app_tx_length)
    {
      tls_result = mbedtls_ssl_write(
          &connection->ssl,
          &connection->app_tx[connection->app_tx_offset],
          connection->app_tx_length - connection->app_tx_offset);
      if (tls_result > 0)
      {
        connection->app_tx_offset += (size_t)tls_result;
        if (connection->app_tx_offset == connection->app_tx_length)
        {
          connection->app_tx_offset = 0U;
          connection->app_tx_length = 0U;
        }
        continue;
      }
      if ((tls_result == MBEDTLS_ERR_SSL_WANT_READ) ||
          (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE))
      {
        return ERR_OK;
      }
      return TCP_Echo_Abort(connection);
    }

    if (connection->close_requested != 0U)
    {
      tls_result = mbedtls_ssl_close_notify(&connection->ssl);
      if (tls_result == 0)
      {
        return TCP_Echo_Close(connection);
      }
      if ((tls_result == MBEDTLS_ERR_SSL_WANT_READ) ||
          (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE))
      {
        return ERR_OK;
      }
      return TCP_Echo_Abort(connection);
    }

    tls_result = mbedtls_ssl_read(&connection->ssl,
                                  application_data,
                                  sizeof(application_data));
    if (tls_result > 0)
    {
      connection->poll_count = 0U;
      TCP_Echo_ProcessApplicationData(connection,
                                      application_data,
                                      (size_t)tls_result);
      continue;
    }
    if ((tls_result == MBEDTLS_ERR_SSL_WANT_READ) ||
        (tls_result == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
      return ERR_OK;
    }
    if ((tls_result == 0) ||
        (tls_result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY))
    {
      connection->close_requested = 1U;
      continue;
    }
    return TCP_Echo_Abort(connection);
  }
  return ERR_OK;
}

static int TCP_Echo_TlsSend(void *context, const unsigned char *buffer,
                            size_t length)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)context;
  u16_t send_length;
  u16_t send_space;
  err_t err;

  if ((connection == NULL) || (connection->pcb == NULL))
  {
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  }
  send_space = tcp_sndbuf(connection->pcb);
  if (send_space == 0U)
  {
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  send_length = (length > (size_t)send_space) ? send_space : (u16_t)length;
  err = tcp_write(connection->pcb,
                  buffer,
                  send_length,
                  TCP_WRITE_FLAG_COPY);
  if (err == ERR_MEM)
  {
    return MBEDTLS_ERR_SSL_WANT_WRITE;
  }
  if (err != ERR_OK)
  {
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  err = tcp_output(connection->pcb);
  if ((err != ERR_OK) && (err != ERR_MEM))
  {
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }
  return (int)send_length;
}

static int TCP_Echo_TlsReceive(void *context, unsigned char *buffer,
                               size_t length)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)context;
  u16_t copy_length;

  if ((connection == NULL) || (connection->pcb == NULL))
  {
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  }
  if (connection->encrypted_rx == NULL)
  {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  copy_length = connection->encrypted_rx->tot_len;
  if ((size_t)copy_length > length)
  {
    copy_length = (u16_t)length;
  }
  (void)pbuf_copy_partial(connection->encrypted_rx,
                          buffer,
                          copy_length,
                          0U);
  connection->encrypted_rx = pbuf_free_header(connection->encrypted_rx,
                                               copy_length);
  tcp_recved(connection->pcb, copy_length);
  return (int)copy_length;
}

static void TCP_Echo_ProcessApplicationData(TCP_EchoConnection_t *connection,
                                            const u8_t *data,
                                            size_t length)
{
  static const char auth_prefix[] = TCP_ECHO_AUTH_PREFIX;
  static const char auth_token[] = TCP_ECHO_AUTH_TOKEN;
  size_t index = 0U;
  size_t expected_length;
  size_t remaining;
  u8_t valid;

  while ((index < length) &&
         (connection->auth_state == TCP_ECHO_AUTH_PENDING))
  {
    if (data[index] == (u8_t)'\n')
    {
      if ((connection->auth_line_length > 0U) &&
          (connection->auth_line[connection->auth_line_length - 1U] ==
           (u8_t)'\r'))
      {
        connection->auth_line_length--;
      }
      expected_length = (sizeof(auth_prefix) - 1U) +
                        (sizeof(auth_token) - 1U);
      valid = (connection->auth_line_length == expected_length) ? 1U : 0U;
      if (valid != 0U)
      {
        valid = (memcmp(connection->auth_line,
                        auth_prefix,
                        sizeof(auth_prefix) - 1U) == 0) ? 1U : 0U;
      }
      if (valid != 0U)
      {
        valid = (memcmp(&connection->auth_line[sizeof(auth_prefix) - 1U],
                        auth_token,
                        sizeof(auth_token) - 1U) == 0) ? 1U : 0U;
      }
      index++;
      if (valid != 0U)
      {
        connection->auth_state = TCP_ECHO_AUTHENTICATED;
        TCP_Echo_QueueApplicationReply(connection,
                                       TCP_EchoAuthOk,
                                       sizeof(TCP_EchoAuthOk) - 1U);
      }
      else
      {
        connection->auth_state = TCP_ECHO_AUTH_REJECTED;
        connection->close_requested = 1U;
        TCP_Echo_QueueApplicationReply(connection,
                                       TCP_EchoAuthFailed,
                                       sizeof(TCP_EchoAuthFailed) - 1U);
      }
    }
    else if (connection->auth_line_length <
             TCP_ECHO_AUTH_LINE_MAX_LENGTH)
    {
      connection->auth_line[connection->auth_line_length] = data[index];
      connection->auth_line_length++;
      index++;
    }
    else
    {
      connection->auth_state = TCP_ECHO_AUTH_REJECTED;
      connection->close_requested = 1U;
      TCP_Echo_QueueApplicationReply(connection,
                                     TCP_EchoAuthFailed,
                                     sizeof(TCP_EchoAuthFailed) - 1U);
    }
  }

  if ((connection->auth_state == TCP_ECHO_AUTHENTICATED) &&
      (index < length))
  {
    remaining = length - index;
    if ((connection->app_tx_length + remaining) <=
        sizeof(connection->app_tx))
    {
      (void)memcpy(&connection->app_tx[connection->app_tx_length],
                   &data[index],
                   remaining);
      connection->app_tx_length += remaining;
    }
    else
    {
      connection->close_requested = 1U;
    }
  }
}

static void TCP_Echo_QueueApplicationReply(TCP_EchoConnection_t *connection,
                                           const char *reply,
                                           size_t length)
{
  if ((connection->app_tx_length + length) <= sizeof(connection->app_tx))
  {
    (void)memcpy(&connection->app_tx[connection->app_tx_length],
                 reply,
                 length);
    connection->app_tx_length += length;
  }
  else
  {
    connection->close_requested = 1U;
  }
}

static err_t TCP_Echo_Close(TCP_EchoConnection_t *connection)
{
  struct tcp_pcb *pcb = connection->pcb;
  err_t err;

  tcp_arg(pcb, NULL);
  tcp_recv(pcb, NULL);
  tcp_sent(pcb, NULL);
  tcp_err(pcb, NULL);
  tcp_poll(pcb, NULL, 0U);
  err = tcp_close(pcb);
  if (err == ERR_OK)
  {
    connection->pcb = NULL;
    TCP_Echo_FreeConnection(connection);
    return ERR_OK;
  }
  if (err == ERR_MEM)
  {
    tcp_arg(pcb, connection);
    tcp_recv(pcb, TCP_Echo_Receive);
    tcp_sent(pcb, TCP_Echo_Sent);
    tcp_err(pcb, TCP_Echo_Error);
    tcp_poll(pcb, TCP_Echo_Poll, TCP_ECHO_POLL_INTERVAL);
    return ERR_OK;
  }
  connection->pcb = pcb;
  return TCP_Echo_Abort(connection);
}

static err_t TCP_Echo_Abort(TCP_EchoConnection_t *connection)
{
  struct tcp_pcb *pcb = connection->pcb;

  if (pcb != NULL)
  {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0U);
    connection->pcb = NULL;
  }
  TCP_Echo_FreeConnection(connection);
  if (pcb != NULL)
  {
    tcp_abort(pcb);
  }
  return ERR_ABRT;
}

static void TCP_Echo_FreeConnection(TCP_EchoConnection_t *connection)
{
  if (connection != NULL)
  {
    if (connection->encrypted_rx != NULL)
    {
      pbuf_free(connection->encrypted_rx);
      connection->encrypted_rx = NULL;
    }
    mbedtls_ssl_free(&connection->ssl);
    (void)memset(connection, 0, sizeof(*connection));
    TCP_EchoConnectionActive = 0U;
  }
}

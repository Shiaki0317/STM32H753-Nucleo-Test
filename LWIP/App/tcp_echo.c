/**
  ******************************************************************************
  * @file    tcp_echo.c
  * @brief   TCP echo server implemented with the lwIP Raw API.
  ******************************************************************************
  */

#include "tcp_echo.h"

#include "lwip/ip_addr.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#define TCP_ECHO_POLL_INTERVAL 2U
#define TCP_ECHO_BACKLOG       2U

typedef struct
{
  struct pbuf *pending;
  u8_t close_pending;
} TCP_EchoConnection_t;

static struct tcp_pcb *TCP_EchoListenPcb;

static err_t TCP_Echo_Accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t TCP_Echo_Receive(void *arg, struct tcp_pcb *tpcb,
                             struct pbuf *p, err_t err);
static err_t TCP_Echo_Sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t TCP_Echo_Poll(void *arg, struct tcp_pcb *tpcb);
static void TCP_Echo_Error(void *arg, err_t err);
static err_t TCP_Echo_Flush(struct tcp_pcb *tpcb,
                            TCP_EchoConnection_t *connection);
static err_t TCP_Echo_Abort(struct tcp_pcb *tpcb,
                            TCP_EchoConnection_t *connection);
static void TCP_Echo_FreeConnection(TCP_EchoConnection_t *connection);

/**
  * @brief  Start listening for IPv4 TCP Echo connections.
  */
err_t TCP_Echo_Init(void)
{
  struct tcp_pcb *pcb;
  struct tcp_pcb *listen_pcb;
  err_t err;

  if (TCP_EchoListenPcb != NULL)
  {
    return ERR_ALREADY;
  }

  pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
  if (pcb == NULL)
  {
    return ERR_MEM;
  }

  err = tcp_bind(pcb, IP_ADDR_ANY, TCP_ECHO_PORT);
  if (err != ERR_OK)
  {
    tcp_abort(pcb);
    return err;
  }

  listen_pcb = tcp_listen_with_backlog_and_err(pcb, TCP_ECHO_BACKLOG, &err);
  if (listen_pcb == NULL)
  {
    /* The original PCB remains valid when conversion to a listen PCB fails. */
    tcp_abort(pcb);
    return err;
  }

  TCP_EchoListenPcb = listen_pcb;
  tcp_accept(TCP_EchoListenPcb, TCP_Echo_Accept);

  return ERR_OK;
}

/**
  * @brief  Allocate per-connection state and register Raw API callbacks.
  */
static err_t TCP_Echo_Accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
  TCP_EchoConnection_t *connection;

  LWIP_UNUSED_ARG(arg);

  if ((err != ERR_OK) || (newpcb == NULL))
  {
    return ERR_VAL;
  }

  connection = (TCP_EchoConnection_t *)mem_calloc(1U,
                                                   sizeof(TCP_EchoConnection_t));
  if (connection == NULL)
  {
    tcp_abort(newpcb);
    return ERR_ABRT;
  }

  tcp_arg(newpcb, connection);
  tcp_recv(newpcb, TCP_Echo_Receive);
  tcp_sent(newpcb, TCP_Echo_Sent);
  tcp_err(newpcb, TCP_Echo_Error);
  tcp_poll(newpcb, TCP_Echo_Poll, TCP_ECHO_POLL_INTERVAL);

  return ERR_OK;
}

/**
  * @brief  Queue received data for echoing, or defer close until queued data is sent.
  */
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
    return TCP_Echo_Abort(tpcb, connection);
  }

  if (p == NULL)
  {
    /* The peer closed its send side. Echo all accepted data before closing. */
    connection->close_pending = 1U;
  }
  else if (connection->pending == NULL)
  {
    connection->pending = p;
  }
  else
  {
    /* Ownership of p is transferred to the pending chain. */
    pbuf_cat(connection->pending, p);
  }

  return TCP_Echo_Flush(tpcb, connection);
}

/**
  * @brief  Continue echoing when acknowledged data releases TCP send space.
  */
static err_t TCP_Echo_Sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)arg;

  LWIP_UNUSED_ARG(len);

  if (connection == NULL)
  {
    tcp_abort(tpcb);
    return ERR_ABRT;
  }

  return TCP_Echo_Flush(tpcb, connection);
}

/**
  * @brief  Retry queued output and deferred close from the TCP timer.
  */
static err_t TCP_Echo_Poll(void *arg, struct tcp_pcb *tpcb)
{
  TCP_EchoConnection_t *connection = (TCP_EchoConnection_t *)arg;

  if (connection == NULL)
  {
    tcp_abort(tpcb);
    return ERR_ABRT;
  }

  return TCP_Echo_Flush(tpcb, connection);
}

/**
  * @brief  Release state after lwIP has already destroyed the TCP PCB.
  */
static void TCP_Echo_Error(void *arg, err_t err)
{
  LWIP_UNUSED_ARG(err);
  TCP_Echo_FreeConnection((TCP_EchoConnection_t *)arg);
}

/**
  * @brief  Copy pending receive data into the TCP send queue.
  */
static err_t TCP_Echo_Flush(struct tcp_pcb *tpcb,
                            TCP_EchoConnection_t *connection)
{
  u16_t write_length;
  u16_t send_space;
  u8_t output_pending = 0U;
  err_t err;

  while (connection->pending != NULL)
  {
    send_space = tcp_sndbuf(tpcb);
    if (send_space == 0U)
    {
      break;
    }

    write_length = connection->pending->len;
    if (write_length > send_space)
    {
      write_length = send_space;
    }

    err = tcp_write(tpcb,
                    connection->pending->payload,
                    write_length,
                    TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM)
    {
      break;
    }
    if (err != ERR_OK)
    {
      return TCP_Echo_Abort(tpcb, connection);
    }

    tcp_recved(tpcb, write_length);
    output_pending = 1U;

    /* Release each DMA-backed Rx pbuf as soon as its bytes are copied. */
    connection->pending = pbuf_free_header(connection->pending, write_length);
  }

  if (output_pending != 0U)
  {
    err = tcp_output(tpcb);
    if ((err != ERR_OK) && (err != ERR_MEM))
    {
      return TCP_Echo_Abort(tpcb, connection);
    }
  }

  if ((connection->close_pending != 0U) && (connection->pending == NULL))
  {
    err = tcp_close(tpcb);
    if (err == ERR_OK)
    {
      TCP_Echo_FreeConnection(connection);
      return ERR_OK;
    }
    if (err != ERR_MEM)
    {
      return TCP_Echo_Abort(tpcb, connection);
    }
  }

  return ERR_OK;
}

/**
  * @brief  Abort a live connection and release all application-owned state.
  */
static err_t TCP_Echo_Abort(struct tcp_pcb *tpcb,
                            TCP_EchoConnection_t *connection)
{
  tcp_arg(tpcb, NULL);
  tcp_recv(tpcb, NULL);
  tcp_sent(tpcb, NULL);
  tcp_err(tpcb, NULL);
  tcp_poll(tpcb, NULL, 0U);
  TCP_Echo_FreeConnection(connection);
  tcp_abort(tpcb);

  return ERR_ABRT;
}

/**
  * @brief  Free queued receive data and per-connection state.
  */
static void TCP_Echo_FreeConnection(TCP_EchoConnection_t *connection)
{
  if (connection != NULL)
  {
    if (connection->pending != NULL)
    {
      pbuf_free(connection->pending);
      connection->pending = NULL;
    }
    mem_free(connection);
  }
}

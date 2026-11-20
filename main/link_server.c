/*
  link_server.c - VSCP link protocol server (TCP/IP)

  The W5500 has a socket per connection: sockets 2, 3 and 4 all listen on
  the link protocol port, so up to three clients can be connected. Each
  client has a link protocol context and is polled from the main loop. Events the client
  sends are given to the node (vscp_core_post); events from the node are put
  in the out queue of every logged in client whose filter lets them through,
  and the client gets them with 'retr' or in a 'rcvloop'.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_tcpiplink

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "socket.h"
#include "wizchip_conf.h"

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-fifo.h>
#include <vscp-firmware-helper.h>
#include <vscp-link-protocol.h>
#include <vscp.h>

#include "blinky.h"

#if BLINKY_LINK_PORT > 0

#if (BLINKY_LINK_MAX_CLIENTS < 1) || (BLINKY_LINK_MAX_CLIENTS > 3)
#error "BLINKY_LINK_MAX_CLIENTS must be 1-3 (W5500 sockets 2-4)"
#endif

#define LINK_FIFO_SIZE     16
#define LINK_RCVLOOP_OK_MS 1000 // '+OK' this often in a receive loop without events

#define LINK_WELCOME                                                                                                   \
  "Welcome to the VSCP W55RP20-EVB-Pico Blinky demo node\r\n"                                                               \
  "Copyright (C) 2026 Ake Hedman, the VSCP project, https://www.vscp.org\r\n"                                         \
  "+OK\r\n"

#define LINK_MSG_MAX_CLIENTS "-OK - Max number of clients reached. Disconnecting.\r\n"

typedef struct {
  vscp_link_ctx_t link;                             // Link protocol context
  uint8_t sn;                                       // W5500 socket
  int used;                                         // A client is connected
  int close;                                        // Close the connection
  char buf[THIS_FIRMWARE_TCPIP_LINK_MAX_BUFFER];    // Received text
  size_t filled;
} client_t;

static client_t s_clients[BLINKY_LINK_MAX_CLIENTS];
static bool s_started;
static int s_next_id = 1;

// ****************************************************************************
//                           Link protocol callbacks
// ****************************************************************************

static int
cb_write_client(vscp_link_ctx_t *pctx, const char *msg)
{
  size_t len = strlen(msg);
  while (len) {
    int32_t n = send((uint8_t) pctx->sock, (uint8_t *) msg, (uint16_t) len);
    if (n <= 0) {
      ((client_t *) pctx->user_data)->close = 1;
      return VSCP_ERROR_WRITE_ERROR;
    }
    msg += n;
    len -= n;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_disconnect(vscp_link_ctx_t *pctx)
{
  ((client_t *) pctx->user_data)->close = 1;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_quit(vscp_link_ctx_t *pctx)
{
  cb_write_client(pctx, VSCP_LINK_MSG_GOODBYE);
  ((client_t *) pctx->user_data)->close = 1;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_welcome(vscp_link_ctx_t *pctx)
{
  return cb_write_client(pctx, LINK_WELCOME);
}

static int
cb_check_user(vscp_link_ctx_t *pctx, const char *arg)
{
  while (*arg && (' ' == *arg)) {
    arg++;
  }
  strncpy(pctx->user, arg, sizeof(pctx->user) - 1);
  pctx->user[sizeof(pctx->user) - 1] = '\0';
  return cb_write_client(pctx, VSCP_LINK_MSG_USENAME_OK);
}

static int
cb_check_password(vscp_link_ctx_t *pctx, const char *arg)
{
  if ('\0' == pctx->user[0]) {
    cb_write_client(pctx, VSCP_LINK_MSG_NEED_USERNAME);
    return VSCP_ERROR_CREDENTIALS;
  }
  while (*arg && (' ' == *arg)) {
    arg++;
  }
  if ((0 == strcmp(pctx->user, BLINKY_LINK_USER)) && (0 == strcmp(arg, BLINKY_LINK_PASSWORD))) {
    pctx->bValidated = 1;
    pctx->privLevel  = 15;
    printf("Link client %d: logged in as %s\n", pctx->id, pctx->user);
    return cb_write_client(pctx, VSCP_LINK_MSG_PASSWORD_OK);
  }
  pctx->user[0]    = '\0';
  pctx->bValidated = 0;
  pctx->privLevel  = 0;
  printf("Link client %d: wrong user or password\n", pctx->id);
  cb_write_client(pctx, VSCP_LINK_MSG_PASSWORD_ERROR);
  return VSCP_ERROR_CREDENTIALS;
}

static int
cb_check_authenticated(vscp_link_ctx_t *pctx)
{
  return pctx->bValidated ? VSCP_ERROR_SUCCESS : VSCP_ERROR_INVALID_PERMISSION;
}

static int
cb_check_privilege(vscp_link_ctx_t *pctx, uint8_t priv)
{
  return (pctx->privLevel >= priv) ? VSCP_ERROR_SUCCESS : VSCP_ERROR_INVALID_PERMISSION;
}

// An event from the client: give it to the node (which owns it on success)
static int
cb_send(vscp_link_ctx_t *pctx, vscp_event_t *pev)
{
  uint16_t size = pev->sizeData;
  if (VSCP_ERROR_SUCCESS != vscp_core_post(pev)) {
    pctx->statistics.cntOverruns++;
    return VSCP_ERROR_TRM_FULL; // The library deletes the event
  }
  pctx->statistics.cntReceiveFrames++;
  pctx->statistics.cntReceiveData += size;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_chkdata(vscp_link_ctx_t *pctx, uint16_t *pcount)
{
  *pcount = LINK_FIFO_SIZE - vscp_fifo_getFree(&pctx->fifoEventsOut);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_clrall(vscp_link_ctx_t *pctx)
{
  vscp_fifo_clear(&pctx->fifoEventsOut); // Deletes the events
  return VSCP_ERROR_SUCCESS;
}

static int
cb_retr(vscp_link_ctx_t *pctx, vscp_event_t **ppev)
{
  if (!vscp_fifo_read(&pctx->fifoEventsOut, ppev) || (NULL == *ppev)) {
    return VSCP_ERROR_RCV_EMPTY;
  }
  pctx->statistics.cntTransmitFrames++;
  pctx->statistics.cntTransmitData += (*ppev)->sizeData;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_rcvloop(vscp_link_ctx_t *pctx, vscp_event_t **ppev)
{
  int rv = cb_retr(pctx, ppev);
  if (VSCP_ERROR_SUCCESS == rv) {
    pctx->last_rcvloop_time = millis();
    return rv;
  }
  // '+OK' every second when there are no events
  if ((millis() - pctx->last_rcvloop_time) >= LINK_RCVLOOP_OK_MS) {
    pctx->last_rcvloop_time = millis();
    return VSCP_ERROR_TIMEOUT;
  }
  return rv;
}

static int
cb_enable_rcvloop(vscp_link_ctx_t *pctx, int enable)
{
  pctx->bRcvLoop          = enable ? 1 : 0;
  pctx->last_rcvloop_time = millis();
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_rcvloop_status(vscp_link_ctx_t *pctx, int *pstatus)
{
  *pstatus = pctx->bRcvLoop;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_guid(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  memcpy(pguid, pctx->guid, 16);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_guid(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  memcpy(pctx->guid, pguid, 16); // GUID of this channel
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_version(vscp_link_ctx_t *pctx, uint8_t *pversion)
{
  pversion[0] = THIS_FIRMWARE_MAJOR_VERSION;
  pversion[1] = THIS_FIRMWARE_MINOR_VERSION;
  pversion[2] = THIS_FIRMWARE_RELEASE_VERSION;
  pversion[3] = THIS_FIRMWARE_BUILD_VERSION;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_channel_id(vscp_link_ctx_t *pctx, uint16_t *pchid)
{
  *pchid = (uint16_t) pctx->id;
  return VSCP_ERROR_SUCCESS;
}

static int
cb_wcyd(vscp_link_ctx_t *pctx, uint64_t *pwcyd)
{
  *pwcyd = VSCP_SERVER_CAPABILITY_TCPIP | VSCP_SERVER_CAPABILITY_DECISION_MATRIX | VSCP_SERVER_CAPABILITY_IP4;
#if BLINKY_UDP_PORT > 0
  *pwcyd |= VSCP_SERVER_CAPABILITY_UDP;
#endif
  if (strlen(BLINKY_MULTICAST_GROUP)) {
    *pwcyd |= VSCP_SERVER_CAPABILITY_MULTICAST_CHANNEL;
  }
  if (BLINKY_LINK_MAX_CLIENTS > 1) {
    *pwcyd |= VSCP_SERVER_CAPABILITY_TWO_CONNECTIONS;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_statistics(vscp_link_ctx_t *pctx, vscp_statistics_t *ps)
{
  memcpy(ps, &pctx->statistics, sizeof(vscp_statistics_t));
  return VSCP_ERROR_SUCCESS;
}

static int
cb_info(vscp_link_ctx_t *pctx, vscp_status_t *ps)
{
  memcpy(ps, &pctx->status, sizeof(vscp_status_t));
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_filter(vscp_link_ctx_t *pctx, vscpEventFilter *pf)
{
  pctx->filter.filter_class    = pf->filter_class;
  pctx->filter.filter_type     = pf->filter_type;
  pctx->filter.filter_priority = pf->filter_priority;
  memcpy(pctx->filter.filter_GUID, pf->filter_GUID, 16);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_set_mask(vscp_link_ctx_t *pctx, vscpEventFilter *pf)
{
  pctx->filter.mask_class    = pf->mask_class;
  pctx->filter.mask_type     = pf->mask_type;
  pctx->filter.mask_priority = pf->mask_priority;
  memcpy(pctx->filter.mask_GUID, pf->mask_GUID, 16);
  return VSCP_ERROR_SUCCESS;
}

static uint16_t
cb_get_interface_count(vscp_link_ctx_t *pctx)
{
  return 1;
}

static int
cb_get_interface(vscp_link_ctx_t *pctx, uint16_t idx, vscp_interface_info_t *pif)
{
  if (0 != idx) {
    return VSCP_ERROR_UNKNOWN_ITEM;
  }
  pif->idx  = idx;
  pif->type = VSCP_INTERFACE_TYPE_LEVEL2DRV;
  memcpy(pif->guid, vscp_core_guid(), 16);
  strncpy(pif->description, "Blinky node", sizeof(pif->description) - 1);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_close_interface(vscp_link_ctx_t *pctx, uint8_t *pguid)
{
  return VSCP_ERROR_NOT_SUPPORTED;
}

static int
cb_shutdown(vscp_link_ctx_t *pctx)
{
  return VSCP_ERROR_NOT_SUPPORTED;
}

static int
cb_restart(vscp_link_ctx_t *pctx)
{
  cb_write_client(pctx, VSCP_LINK_MSG_OK);
  vscp_core_request_restart(); // Saves pending changes first
  return VSCP_ERROR_SUCCESS;
}

static const vscp_link_ops_t s_link_ops = {
  .write_client        = cb_write_client,
  .disconnect          = cb_disconnect,
  .quit                = cb_quit,
  .welcome             = cb_welcome,
  .check_user          = cb_check_user,
  .check_password      = cb_check_password,
  .check_authenticated = cb_check_authenticated,
  .check_privilege     = cb_check_privilege,
  .send                = cb_send,
  .chkdata             = cb_chkdata,
  .clrall              = cb_clrall,
  .retr                = cb_retr,
  .rcvloop             = cb_rcvloop,
  .enable_rcvloop      = cb_enable_rcvloop,
  .get_rcvloop_status  = cb_get_rcvloop_status,
  .get_guid            = cb_get_guid,
  .set_guid            = cb_set_guid,
  .get_version         = cb_get_version,
  .get_channel_id      = cb_get_channel_id,
  .wcyd                = cb_wcyd,
  .statistics          = cb_statistics,
  .info                = cb_info,
  .set_filter          = cb_set_filter,
  .set_mask            = cb_set_mask,
  .get_interface_count = cb_get_interface_count,
  .get_interface       = cb_get_interface,
  .close_interface     = cb_close_interface,
  .shutdown            = cb_shutdown,
  .restart             = cb_restart,
};

// ****************************************************************************
//                                  Clients
// ****************************************************************************

static void
client_open(client_t *c)
{
  printf("Link client %d: connected\n", s_next_id);
  memset(&c->link, 0, sizeof(c->link));
  c->close          = 0;
  c->filled         = 0;
  c->link.ops       = &s_link_ops;
  c->link.id        = s_next_id++;
  c->link.sock      = c->sn;
  c->link.user_data = c;
  memcpy(c->link.guid, vscp_core_guid(), 16);
  vscp_fifo_init(&c->link.fifoEventsOut, LINK_FIFO_SIZE);
  c->used = 1;
  vscp_link_connect(&c->link); // Welcome
}

static void
client_close(client_t *c)
{
  if (c->used) {
    printf("Link client %d: disconnected\n", c->link.id);
    c->used = 0;
    vscp_fifo_deinit(&c->link.fifoEventsOut); // Deletes the queued events
  }
}

static void
client_work(client_t *c)
{
  switch (getSn_SR(c->sn)) {

    case SOCK_ESTABLISHED: {
      if (!c->used) {
        client_open(c);
      }

      // Received text: handle every complete line
      uint16_t avail = getSn_RX_RSR(c->sn);
      if (avail) {
        size_t room = sizeof(c->buf) - 1 - c->filled;
        int32_t n   = recv(c->sn, (uint8_t *) c->buf + c->filled, (uint16_t) ((avail < room) ? avail : room));
        if (n > 0) {
          c->filled += n;
          c->buf[c->filled] = '\0';
          char *eol;
          while (!c->close && (NULL != (eol = strchr(c->buf, '\n')))) {
            *eol        = '\0';
            size_t used = eol - c->buf + 1;
            vscp_link_parser(&c->link, c->buf);
            memmove(c->buf, c->buf + used, c->filled - used + 1);
            c->filled -= used;
          }
          if (c->filled >= sizeof(c->buf) - 1) {
            c->filled = 0; // Too long line
            cb_write_client(&c->link, VSCP_LINK_MSG_BUFFER_FULL);
          }
        }
      }

      // Events to a client in a receive loop
      if (c->used && !c->close) {
        vscp_link_idle_worker(&c->link);
      }

      if (c->close) {
        disconnect(c->sn);
        client_close(c);
      }
    } break;

    case SOCK_CLOSE_WAIT: // Closed by the client
      disconnect(c->sn);
      client_close(c);
      break;

    case SOCK_CLOSED:
      client_close(c);
      if (c->sn == socket(c->sn, Sn_MR_TCP, BLINKY_LINK_PORT, 0)) {
        listen(c->sn);
      }
      break;

    default: // SOCK_INIT, SOCK_LISTEN, ... (waiting)
      break;
  }
}

// ****************************************************************************
//                                Public API
// ****************************************************************************

void
link_server_send_event(const vscp_event_t *pev)
{
  for (int i = 0; i < BLINKY_LINK_MAX_CLIENTS; i++) {
    client_t *c = &s_clients[i];
    if (!c->used || !c->link.bValidated) {
      continue;
    }
    if (vscp_fwhlp_doLevel2Filter(pev, &c->link.filter)) {
      vscp_event_t *pnew = vscp_fwhlp_mkEventCopy(pev);
      if ((NULL == pnew) || !vscp_fifo_write(&c->link.fifoEventsOut, pnew)) {
        c->link.statistics.cntOverruns++;
        vscp_fwhlp_deleteEvent(&pnew);
      }
    }
  }
}

void
link_server_start(void)
{
  for (int i = 0; i < BLINKY_LINK_MAX_CLIENTS; i++) {
    s_clients[i].sn = SOCKET_LINK_BASE + i;
  }
  s_started = true;
  printf("VSCP link protocol server on port %d (%d clients)\n", BLINKY_LINK_PORT, BLINKY_LINK_MAX_CLIENTS);
}

void
link_server_work(void)
{
  if (s_started) {
    for (int i = 0; i < BLINKY_LINK_MAX_CLIENTS; i++) {
      client_work(&s_clients[i]);
    }
  }
}

#else // BLINKY_LINK_PORT

void
link_server_send_event(const vscp_event_t *pev)
{
}

void
link_server_start(void)
{
}

void
link_server_work(void)
{
}

#endif

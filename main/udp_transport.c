/*
  udp_transport.c - VSCP frames over UDP and the VSCP multicast group

  Every datagram is one VSCP frame (version 0, not encrypted):
  packet type, header, timestamp, date/time, class, type, GUID, data size,
  data and CRC. Received frames are given to the node; events from the node
  are sent as frames to the UDP remote host (broadcast by default) and to
  the multicast group.

  W5500 socket 5 is used for UDP, socket 6 for multicast. The W5500 joins
  the group (IGMP) itself when the multicast socket is opened.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_udp
  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_multicast

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "socket.h"
#include "wizchip_conf.h"

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-firmware-helper.h>
#include <vscp.h>

#include "blinky.h"

#define UDP_MAX_FRAME (1 + 35 + VSCP_MAX_DATA + 2) // Type, header, data, CRC

typedef struct {
  const char *name;
  uint8_t sn;
  bool open;
  uint8_t to[4]; // Events from the node are sent here
  uint16_t port;
} channel_t;

static channel_t s_udp   = { .name = "UDP", .sn = SOCKET_UDP };
static channel_t s_mcast = { .name = "multicast", .sn = SOCKET_MULTICAST };

static bool
parse_ip(uint8_t *ip, const char *str)
{
  unsigned a, b, c, d;
  if ((4 != sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d)) || (a > 255) || (b > 255) || (c > 255) || (d > 255)) {
    return false;
  }
  ip[0] = a;
  ip[1] = b;
  ip[2] = c;
  ip[3] = d;
  return true;
}

// A frame from the network: give the event to the node
static void
receive(channel_t *ch)
{
  static uint8_t buf[UDP_MAX_FRAME + 64];
  uint8_t from[4];
  uint16_t port;
  int32_t len = recvfrom(ch->sn, buf, sizeof(buf), from, &port);
  if (len <= 0) {
    return;
  }

  // The low nibble of the packet type is the encryption (not supported)
  if (buf[0] & 0x0f) {
    printf("%s: encrypted frame from %d.%d.%d.%d ignored\n", ch->name, from[0], from[1], from[2], from[3]);
    return;
  }

  vscp_event_t *pev = vscp_fwhlp_newEvent();
  if (NULL == pev) {
    return;
  }
  int rv = vscp_fwhlp_getEventFromFrame(pev, buf, len);
  if (VSCP_ERROR_SUCCESS != rv) {
    printf("%s: bad frame from %d.%d.%d.%d (%d)\n", ch->name, from[0], from[1], from[2], from[3], rv);
    vscp_fwhlp_deleteEvent(&pev);
    return;
  }

  // Our own events (broadcast/multicast that comes back)
  if ((0 == memcmp(pev->GUID, vscp_core_guid(), 16)) || (VSCP_ERROR_SUCCESS != vscp_core_post(pev))) {
    vscp_fwhlp_deleteEvent(&pev);
  }
}

void
udp_transport_send_event(const vscp_event_t *pev)
{
  static uint8_t frame[UDP_MAX_FRAME];
  if (!s_udp.open && !s_mcast.open) {
    return;
  }
  size_t size = vscp_fwhlp_getFrameSizeFromEvent((vscp_event_t *) pev);
  if ((0 == size) || (size > sizeof(frame)) ||
      (VSCP_ERROR_SUCCESS != vscp_fwhlp_writeEventToFrame(frame, sizeof(frame), 0, pev))) {
    printf("Could not make a frame for event %d/%d\n", pev->vscp_class, pev->vscp_type);
    return;
  }
  channel_t *channels[] = { &s_udp, &s_mcast };
  for (int i = 0; i < 2; i++) {
    if (channels[i]->open) {
      sendto(channels[i]->sn, frame, (uint16_t) size, channels[i]->to, channels[i]->port);
    }
  }
}

void
udp_transport_start(void)
{
#if BLINKY_UDP_PORT > 0
  if (parse_ip(s_udp.to, BLINKY_UDP_REMOTE_HOST) && (SOCKET_UDP == socket(SOCKET_UDP, Sn_MR_UDP, BLINKY_UDP_PORT, 0))) {
    s_udp.port = BLINKY_UDP_PORT;
    s_udp.open = true;
    printf("UDP: port %d, events to %s\n", BLINKY_UDP_PORT, BLINKY_UDP_REMOTE_HOST);
  }
  else {
    printf("UDP: could not open port %d\n", BLINKY_UDP_PORT);
  }
#endif

  if (parse_ip(s_mcast.to, BLINKY_MULTICAST_GROUP)) {
    // Group address, port and multicast MAC (01:00:5e + low 23 bits of the group)
    uint8_t mac[6] = { 0x01, 0x00, 0x5e, (uint8_t) (s_mcast.to[1] & 0x7f), s_mcast.to[2], s_mcast.to[3] };
    setSn_DIPR(SOCKET_MULTICAST, s_mcast.to);
    setSn_DPORT(SOCKET_MULTICAST, BLINKY_MULTICAST_PORT);
    setSn_DHAR(SOCKET_MULTICAST, mac);
    setSn_TTL(SOCKET_MULTICAST, BLINKY_MULTICAST_TTL);
    if (SOCKET_MULTICAST ==
        socket(SOCKET_MULTICAST, Sn_MR_UDP, BLINKY_MULTICAST_PORT, SF_MULTI_ENABLE | SF_IGMP_VER2)) {
      s_mcast.port = BLINKY_MULTICAST_PORT;
      s_mcast.open = true;
      printf("Multicast: group %s port %d\n", BLINKY_MULTICAST_GROUP, BLINKY_MULTICAST_PORT);
    }
    else {
      printf("Multicast: could not join %s\n", BLINKY_MULTICAST_GROUP);
    }
  }
}

void
udp_transport_work(void)
{
  channel_t *channels[] = { &s_udp, &s_mcast };
  for (int i = 0; i < 2; i++) {
    if (channels[i]->open && (getSn_RX_RSR(channels[i]->sn) > 0)) {
      receive(channels[i]);
    }
  }
}

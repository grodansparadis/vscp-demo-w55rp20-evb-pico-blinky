/*
  mqtt_transport.c - VSCP JSON events over MQTT

  Events from the node are published, one VSCP JSON event per message, on
  the publish topic (default vscp/{guid}/{class}/{type}). VSCP JSON events
  published on the subscribe topic (default vscp/{guid}/in) are given to the
  node.

  The MQTT client is the Paho embedded client in the WIZnet ioLibrary, on
  W5500 socket 7. The TCP connection is made without blocking (the
  ioLibrary ConnectNetwork() blocks until the connect times out, about 30
  seconds, when the broker is not there) and is made again when it is lost.

  https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_mqtt

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "MQTTClient.h"
#include "mqtt_interface.h"
#include "socket.h"
#include "wizchip_conf.h"

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-firmware-helper.h>
#include <vscp.h>

#include "blinky.h"

#define MQTT_JSON_MAX      1024 // A VSCP JSON event with 8 data bytes is about 250 bytes
#define MQTT_TOPIC_MAX     128
#define MQTT_RETRY_MS      5000
#define MQTT_CONNECT_MS    5000 // TCP connect timeout
#define MQTT_COMMAND_MS    2000 // CONNECT/SUBSCRIBE reply timeout
#define MQTT_KEEPALIVE_S   60
#define MQTT_LOCAL_PORT    50000

typedef enum { MQTT_OFF, MQTT_IDLE, MQTT_CONNECTING, MQTT_CONNECTED } mqtt_state_t;

static mqtt_state_t s_state = MQTT_OFF;
static uint8_t s_broker[4];
static uint32_t s_time;          // Time of the last state change
static Network s_network;
static MQTTClient s_client;
static uint8_t s_txbuf[MQTT_JSON_MAX + MQTT_TOPIC_MAX + 16];
static uint8_t s_rxbuf[MQTT_JSON_MAX + MQTT_TOPIC_MAX + 16];
static char s_json[MQTT_JSON_MAX + 1];
static char s_guid[48];                   // GUID as a string, FF:FF:...
static char s_sub_topic[MQTT_TOPIC_MAX]; // Subscribe topic
static char s_client_id[40];

// Make a topic from a template: {guid}, {class} and {type} are replaced
static void
make_topic(char *topic, size_t size, const char *tmpl, const vscp_event_t *pev)
{
  size_t pos = 0;
  topic[0]   = '\0';
  while (*tmpl && (pos < size - 1)) {
    const char *value = NULL;
    char num[8];
    if (0 == strncmp(tmpl, "{guid}", 6)) {
      value = s_guid;
      tmpl += 6;
    }
    else if ((NULL != pev) && (0 == strncmp(tmpl, "{class}", 7))) {
      snprintf(num, sizeof(num), "%u", pev->vscp_class);
      value = num;
      tmpl += 7;
    }
    else if ((NULL != pev) && (0 == strncmp(tmpl, "{type}", 6))) {
      snprintf(num, sizeof(num), "%u", pev->vscp_type);
      value = num;
      tmpl += 6;
    }
    if (NULL != value) {
      pos += snprintf(topic + pos, size - pos, "%s", value);
      if (pos >= size) {
        pos = size - 1;
      }
    }
    else {
      topic[pos++] = *tmpl++;
      topic[pos]   = '\0';
    }
  }
}

// A VSCP JSON event from the broker: give it to the node
static void
handle_message(const char *json)
{
  vscp_event_t *pev = vscp_fwhlp_newEvent();
  if (NULL == pev) {
    return;
  }
  int rv = vscp_fwhlp_parse_json(pev, json);
  if (VSCP_ERROR_SUCCESS != rv) {
    printf("MQTT: not a VSCP JSON event (%d): %.80s\n", rv, json);
    vscp_fwhlp_deleteEvent(&pev);
    return;
  }
  if (VSCP_ERROR_SUCCESS != vscp_core_post(pev)) {
    vscp_fwhlp_deleteEvent(&pev);
  }
}

// Paho: a message on the subscribe topic
static void
message_arrived(MessageData *md)
{
  MQTTMessage *msg = md->message;
  if (msg->payloadlen >= sizeof(s_json)) {
    printf("MQTT: message too long (%u bytes)\n", (unsigned) msg->payloadlen);
    return;
  }
  memcpy(s_json, msg->payload, msg->payloadlen);
  s_json[msg->payloadlen] = '\0';
  handle_message(s_json); // Queued; handled in vscp_core_work()
}

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

static void
lost(const char *why)
{
  printf("MQTT: %s, connecting again in %d s\n", why, MQTT_RETRY_MS / 1000);
  close(SOCKET_MQTT);
  s_client.isconnected = 0;
  s_state              = MQTT_IDLE;
  s_time               = millis();
}

void
mqtt_transport_send_event(const vscp_event_t *pev)
{
  char topic[MQTT_TOPIC_MAX];
  static char json[MQTT_JSON_MAX];
  if (MQTT_CONNECTED != s_state) {
    return;
  }
  if (VSCP_ERROR_SUCCESS != vscp_fwhlp_create_json(json, sizeof(json), pev)) {
    printf("MQTT: could not make JSON for event %d/%d\n", pev->vscp_class, pev->vscp_type);
    return;
  }
  // One line: the helper pretty prints (no string in the event has tabs or newlines)
  char *w = json;
  for (char *r = json; *r; r++) {
    if (('\n' != *r) && ('\t' != *r) && ('\r' != *r)) {
      *w++ = *r;
    }
  }
  *w = '\0';
  make_topic(topic, sizeof(topic), BLINKY_MQTT_PUBLISH_TOPIC, pev);

  MQTTMessage msg = { .qos = QOS0, .retained = 0, .payload = json, .payloadlen = strlen(json) };
  if (MQTTPublish(&s_client, topic, &msg) < 0) {
    lost("publish failed");
  }
}

void
mqtt_transport_start(void)
{
  const uint8_t *guid = vscp_core_guid();
  if (!parse_ip(s_broker, BLINKY_MQTT_BROKER)) {
    return; // MQTT off
  }
  snprintf(s_guid, sizeof(s_guid), "%02X", guid[0]);
  for (int i = 1; i < 16; i++) {
    snprintf(s_guid + strlen(s_guid), sizeof(s_guid) - strlen(s_guid), ":%02X", guid[i]);
  }
  make_topic(s_sub_topic, sizeof(s_sub_topic), BLINKY_MQTT_SUBSCRIBE_TOPIC, NULL);
  snprintf(s_client_id,
           sizeof(s_client_id),
           "vscp-blinky-%02x%02x%02x%02x%02x%02x",
           guid[8],
           guid[9],
           guid[10],
           guid[11],
           guid[12],
           guid[13]);
  s_state = MQTT_IDLE;
  s_time  = millis() - MQTT_RETRY_MS; // Connect at once
}

void
mqtt_transport_work(void)
{
  switch (s_state) {

    case MQTT_OFF:
      break;

    case MQTT_IDLE:
      if ((millis() - s_time) >= MQTT_RETRY_MS) {
        // Non-blocking TCP connect
        if ((SOCKET_MQTT == socket(SOCKET_MQTT, Sn_MR_TCP, MQTT_LOCAL_PORT + (millis() % 1000), SF_IO_NONBLOCK))) {
          connect(SOCKET_MQTT, s_broker, BLINKY_MQTT_PORT);
          s_state = MQTT_CONNECTING;
        }
        s_time = millis();
      }
      break;

    case MQTT_CONNECTING: {
      uint8_t sr = getSn_SR(SOCKET_MQTT);
      if (SOCK_ESTABLISHED == sr) {
        uint8_t mode = SOCK_IO_BLOCK; // Paho reads and writes in blocking mode
        ctlsocket(SOCKET_MQTT, CS_SET_IOMODE, &mode);
        NewNetwork(&s_network, SOCKET_MQTT);
        MQTTClientInit(&s_client, &s_network, MQTT_COMMAND_MS, s_txbuf, sizeof(s_txbuf), s_rxbuf, sizeof(s_rxbuf));

        MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
        data.MQTTVersion            = 4;
        data.clientID.cstring       = s_client_id;
        data.keepAliveInterval      = MQTT_KEEPALIVE_S;
        data.cleansession           = 1;
        if (strlen(BLINKY_MQTT_USER)) {
          data.username.cstring = BLINKY_MQTT_USER;
          data.password.cstring = BLINKY_MQTT_PASSWORD;
        }
        if (SUCCESSS != MQTTConnect(&s_client, &data)) {
          lost("broker did not accept the connection");
          break;
        }
        if (SUCCESSS != MQTTSubscribe(&s_client, s_sub_topic, QOS0, message_arrived)) {
          lost("subscribe failed");
          break;
        }
        printf("MQTT: connected to %s:%d, subscribed to %s\n", BLINKY_MQTT_BROKER, BLINKY_MQTT_PORT, s_sub_topic);
        s_state = MQTT_CONNECTED;
      }
      else if ((SOCK_CLOSED == sr) || ((millis() - s_time) >= MQTT_CONNECT_MS)) {
        lost("no connection to the broker");
      }
    } break;

    case MQTT_CONNECTED:
      if ((SOCK_ESTABLISHED != getSn_SR(SOCKET_MQTT)) || !s_client.isconnected) {
        lost("connection lost");
      }
      else if (MQTTYield(&s_client, 1) < 0) {
        lost("connection error");
      }
      break;
  }
}

/*
  main.c - VSCP Blinky demo for the WIZnet W55RP20-EVB-Pico

  Starts the W5500 (inside the W55RP20), the VSCP node (blinky.c) and the
  transports: VSCP link protocol server (TCP/IP), UDP, multicast and MQTT.
  Everything runs in the main loop; there is no RTOS.

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include "blinky.h"

int
main(void)
{
  stdio_init_all();
  sleep_ms(1500); // Time to open the USB serial port

  printf("\nVSCP Blinky demo for the W55RP20-EVB-Pico %d.%d.%d\n",
         THIS_FIRMWARE_MAJOR_VERSION,
         THIS_FIRMWARE_MINOR_VERSION,
         THIS_FIRMWARE_RELEASE_VERSION);

  /*
    MAC address: the W5500 has none of its own. A locally administered
    address is made from the unique id of the flash (02:xx:xx:xx:xx:xx).
    GUID: FF:FF:FF:FF:FF:FF:FF:FE:<MAC>:00:00
    https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_globally_unique_identifiers
  */
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  uint8_t mac[6] = { 0x02, id.id[3], id.id[4], id.id[5], id.id[6], id.id[7] };

  uint8_t guid[16];
  memset(guid, 0xff, 7);
  guid[7] = 0xfe;
  memcpy(guid + 8, mac, 6);
  guid[14] = 0;
  guid[15] = 0;

  printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  network_init(mac);  // Sets the system clock for the W5500 PIO SPI
  vscp_core_init(guid);

  bool started = false;
  for (;;) {
    if (network_work() && !started) {
      // The node has an IP address
      link_server_start();
      udp_transport_start();
      mqtt_transport_start();
      started = true;
    }
    if (started) {
      link_server_work();
      udp_transport_work();
      mqtt_transport_work();
    }
    vscp_core_work();
  }
}

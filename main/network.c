/*
  network.c - W5500 (in the W55RP20), DHCP and SNTP

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "port_common.h"
#include "wizchip_conf.h"
#include "wizchip_qspi_pio.h"
#include "wizchip_spi.h"

#include "dhcp.h"
#include "mqtt_interface.h"
#include "sntp.h"
#include "timer.h"

#include <vscp-firmware-helper.h>

#include "blinky.h"

#define DHCP_RETRY_MS  10000 // A new DHCP try after a failure
#define SNTP_RETRY_MS  5000  // A new SNTP request when there is no answer
#define SNTP_EVERY_MS  (3600u * 1000u)
#define SNTP_UTC_INDEX 21 // UTC in the ioLibrary SNTP time zone table
#define LINK_CHECK_MS  500

extern wiznet_spi_handle_t spi_handle; // W55RP20 PIO SPI (wizchip_spi.c)

static wiz_NetInfo s_net = { .dhcp = NETINFO_DHCP };
static uint8_t s_dhcp_buf[548];
static uint8_t s_sntp_buf[48];
static volatile uint32_t s_ms; // Milliseconds since the start (1 ms timer)
static bool s_have_ip;
static bool s_dhcp;
static bool s_chip_ok;   // The W5500 answers
static bool s_link;      // Ethernet link
static bool s_net_up;    // Address set up (DHCP started or static)

uint32_t
millis(void)
{
  return s_ms;
}

// 1 ms timer (interrupt)
static void
timer_1ms(void)
{
  s_ms++;
  MilliTimer_Handler(); // MQTT client timeouts
  if (0 == (s_ms % 1000)) {
    DHCP_time_handler();
  }
}

static bool
parse_ip(uint8_t *ip, const char *str)
{
  unsigned a, b, c, d;
  if ((NULL == str) || (4 != sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d)) || (a > 255) || (b > 255) || (c > 255) ||
      (d > 255)) {
    return false;
  }
  ip[0] = a;
  ip[1] = b;
  ip[2] = c;
  ip[3] = d;
  return true;
}

static void
got_ip(void)
{
  printf("IP address %d.%d.%d.%d\n", s_net.ip[0], s_net.ip[1], s_net.ip[2], s_net.ip[3]);
  vscp_core_set_ip(s_net.ip);
  s_have_ip = true;
}

static void
dhcp_assign(void)
{
  getIPfromDHCP(s_net.ip);
  getGWfromDHCP(s_net.gw);
  getSNfromDHCP(s_net.sn);
  getDNSfromDHCP(s_net.dns);
  s_net.dhcp = NETINFO_DHCP;
  network_initialize(s_net);
  got_ip();
}

static void
dhcp_conflict(void)
{
  printf("DHCP: address conflict\n");
  s_have_ip = false;
}

/*
  The W5500 set up as wizchip_initialize() and wizchip_check() in the WIZnet
  port do it, but without waiting (for ever) for an Ethernet link or a chip
  that does not answer, so the node also works without a network.
*/
static bool
w5500_start(void)
{
  uint8_t memsize[2][8] = { { 2, 2, 2, 2, 2, 2, 2, 2 }, { 2, 2, 2, 2, 2, 2, 2, 2 } };

  (*spi_handle)->frame_end();
  reg_wizchip_spi_cbfunc((*spi_handle)->read_byte, (*spi_handle)->write_byte);
  reg_wizchip_spiburst_cbfunc((*spi_handle)->read_buffer, (*spi_handle)->write_buffer);
  reg_wizchip_cs_cbfunc((*spi_handle)->frame_start, (*spi_handle)->frame_end);

  if (-1 == ctlwizchip(CW_INIT_WIZCHIP, (void *) memsize)) {
    printf("W5500: init failed\n");
    return false;
  }
  if (0x04 != getVERSIONR()) {
    printf("W5500: no answer (version 0x%02x, should be 0x04). Is this a W55RP20?\n", getVERSIONR());
    return false;
  }
  return true;
}

void
network_init(const uint8_t *mac)
{
  // The W5500 PIO SPI timing assumes a 133 MHz system clock
  set_sys_clock_khz(133000, true);
  clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, 133000 * 1000, 133000 * 1000);

  wizchip_spi_initialize();
  wizchip_cris_initialize();
  wizchip_reset();
  s_chip_ok = w5500_start();

  wizchip_1ms_timer_initialize(timer_1ms);

  memcpy(s_net.mac, mac, 6);
  if (s_chip_ok) {
    network_initialize(s_net); // MAC
  }
}

// Set up the address when there is a link (DHCP or static)
static void
net_up(void)
{
  if (parse_ip(s_net.ip, BLINKY_IP)) {
    parse_ip(s_net.sn, BLINKY_NETMASK);
    parse_ip(s_net.gw, BLINKY_GATEWAY);
    s_net.dhcp = NETINFO_STATIC;
    network_initialize(s_net);
    got_ip();
  }
  else {
    s_dhcp     = true;
    s_net.dhcp = NETINFO_DHCP;
    DHCP_init(SOCKET_DHCP, s_dhcp_buf);
    reg_dhcp_cbfunc(dhcp_assign, dhcp_assign, dhcp_conflict);
    printf("DHCP: asking for an address\n");
  }
  s_net_up = true;
}

static void
sntp_work(void)
{
  static uint8_t server[4];
  static bool started;
  static uint32_t last_request, last_set;
  static bool time_set;

  if (!parse_ip(server, BLINKY_SNTP_SERVER)) {
    return; // SNTP off
  }
  if (!started) {
    SNTP_init(SOCKET_SNTP, server, SNTP_UTC_INDEX, s_sntp_buf);
    started      = true;
    last_request = millis();
  }
  if (time_set && ((millis() - last_set) < SNTP_EVERY_MS)) {
    return;
  }

  datetime t;
  if (1 == SNTP_run(&t)) {
    vscp_core_set_time(vscp_fwhlp_to_unix_ns(t.yy, t.mo, t.dd, t.hh, t.mm, t.ss, 0));
    if (!time_set) {
      printf("SNTP: %04d-%02d-%02dT%02d:%02d:%02dZ\n", t.yy, t.mo, t.dd, t.hh, t.mm, t.ss);
    }
    time_set = true;
    last_set = millis();
  }
  else if ((millis() - last_request) > SNTP_RETRY_MS) {
    SNTP_init(SOCKET_SNTP, server, SNTP_UTC_INDEX, s_sntp_buf); // Ask again
    last_request = millis();
  }
}

bool
network_work(void)
{
  static uint32_t dhcp_retry_at, last_link_check;

  if (!s_chip_ok) {
    return false;
  }

  // Ethernet link
  if ((millis() - last_link_check) >= LINK_CHECK_MS) {
    uint8_t link = PHY_LINK_OFF;
    last_link_check = millis();
    ctlwizchip(CW_GET_PHYLINK, (void *) &link);
    if ((PHY_LINK_ON == link) != s_link) {
      s_link = (PHY_LINK_ON == link);
      printf("Ethernet link %s\n", s_link ? "up" : "down");
      if (s_link && !s_net_up) {
        net_up();
      }
    }
  }

  if (s_dhcp) {
    if ((0 == dhcp_retry_at) || ((int32_t) (millis() - dhcp_retry_at) >= 0)) {
      switch (DHCP_run()) {
        case DHCP_FAILED:
          printf("DHCP: no answer, trying again\n");
          s_have_ip     = false;
          dhcp_retry_at = millis() + DHCP_RETRY_MS;
          DHCP_init(SOCKET_DHCP, s_dhcp_buf);
          break;
        default:
          dhcp_retry_at = 0;
          break;
      }
    }
  }

  if (s_have_ip) {
    sntp_work();
  }
  return s_have_ip;
}

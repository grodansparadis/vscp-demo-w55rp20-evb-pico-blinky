/*
  blinky.c - VSCP Blinky demo for the W55RP20-EVB-Pico: the node

  The Level II firmware (vscp-firmware) runs from the main loop
  (vscp_core_work). It handles the VSCP protocol (register read/write, ...),
  runs the decision matrix and does the Blinky work: LED, button and
  millisecond counter. Events from the transports come in through a queue
  (vscp_core_post) and events from the node are given to every transport.

  This file is part of the VSCP (https://www.vscp.org)

  The MIT License (MIT)
  Copyright (C) 2026 Ake Hedman, the VSCP project <info@vscp.org>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "vscp-compiler.h"
#include "vscp-projdefs.h"

#include <vscp-class.h>
#include <vscp-crc.h>
#include <vscp-fifo.h>
#include <vscp-firmware-helper.h>
#include <vscp-firmware-level2.h>
#include <vscp-type.h>
#include <vscp.h>

#include "blinky.h"

#define CORE_QUEUE_SIZE  32
#define BUTTON_POLL_MS   10 // The button is read this often
#define BUTTON_DEBOUNCE  3  // Number of equal button reads for a change

#define REGS_VERSION 1

// Saved registers
typedef struct {
  uint8_t version; // REGS_VERSION
  uint8_t zone;
  uint8_t subzone;
  uint8_t control;
  uint16_t blink_interval;
  uint8_t button_byte0;
  uint8_t button_zone;
  uint8_t button_subzone;
  uint8_t user_id[5];
  uint16_t nickname;
  uint8_t dm[BLINKY_DM_ROWS * BLINKY_DM_ROW_SIZE];
} blinky_regs_t;

static blinky_regs_t s_regs;

static vscp_fifo_t s_queue; // Events from the transports
static vscp_frmw2_firmware_context_t s_fw;

static uint8_t s_status;     // Status register (LED and button bits)
static int s_blinking;       // The LED blinks
static uint32_t s_last_blink; // Time of the last blink toggle
static int s_save_pending;   // Registers changed and not saved yet
static uint32_t s_save_time; // Time of the last change
static int s_restart_requested;
static int64_t s_time_offset_ns; // Unix time - time since start (0 = not set)
static repeating_timer_t s_counter_timer;

static volatile uint32_t s_counter; // Millisecond counter (registers 6-9)
static volatile int s_counter_wrapped;

// ****************************************************************************
//                                  Helpers
// ****************************************************************************

uint64_t
vscp_core_timestamp_ns(void)
{
  uint64_t uptime_ns = time_us_64() * 1000ull;
  return s_time_offset_ns ? (uint64_t) (uptime_ns + s_time_offset_ns) : uptime_ns;
}

void
vscp_core_set_time(uint64_t unix_ns)
{
  s_time_offset_ns = (int64_t) (unix_ns - time_us_64() * 1000ull);
}

const uint8_t *
vscp_core_guid(void)
{
  return s_fw.guid;
}

void
vscp_core_set_ip(const uint8_t *ip)
{
  // IPv4 in the first four bytes, MSB first, the rest zero (as the VSCP daemon does)
  memset(s_fw.ipaddr, 0, sizeof(s_fw.ipaddr));
  memcpy(s_fw.ipaddr, ip, 4);
}

void
vscp_core_request_restart(void)
{
  s_restart_requested = 1;
}

int
vscp_core_post(vscp_event_t *pev)
{
  if (NULL == pev) {
    return VSCP_ERROR_INVALID_POINTER;
  }
  if (!vscp_fifo_write(&s_queue, pev)) {
    return VSCP_ERROR_FIFO_FULL; // The caller still owns the event
  }
  return VSCP_ERROR_SUCCESS;
}

// ****************************************************************************
//                              Saved registers
// ****************************************************************************

static void
regs_defaults(void)
{
  memset(&s_regs, 0, sizeof(s_regs));
  s_regs.version        = REGS_VERSION;
  s_regs.control        = BLINKY_DEFAULT_CONTROL;
  s_regs.blink_interval = BLINKY_DEFAULT_BLINK_INTERVAL;
  s_regs.nickname       = BLINKY_DEFAULT_NICKNAME;
}

static void
regs_load(void)
{
  if (storage_load(&s_regs, sizeof(s_regs)) && (REGS_VERSION == s_regs.version)) {
    return;
  }
  printf("No saved registers, using the defaults\n");
  regs_defaults();
}

static void
regs_save(void)
{
  s_save_pending = 0;
  storage_save(&s_regs, sizeof(s_regs));
}

// Saved a little later, so a burst of register writes is one save
static void
regs_changed(void)
{
  s_save_pending = 1;
  s_save_time    = millis();
}

// Copy the saved values the firmware keeps in its context
static void
regs_to_context(void)
{
  s_fw.zone     = s_regs.zone;
  s_fw.subzone  = s_regs.subzone;
  s_fw.nickname = s_regs.nickname;
  memcpy(s_fw.userId, s_regs.user_id, sizeof(s_fw.userId));
}

// ****************************************************************************
//                                  Blinky
// ****************************************************************************

static int
send_event(uint16_t vscp_class, uint16_t vscp_type, const uint8_t *pdata, uint8_t size)
{
  vscp_event_t ev;
  memset(&ev, 0, sizeof(ev));
  if (VSCP_ERROR_SUCCESS != vscp_frmw2_setup_event(&s_fw, &ev, size)) { // Header, GUID, time, data buffer
    return VSCP_ERROR_MEMORY;
  }
  ev.vscp_class = vscp_class;
  ev.vscp_type  = vscp_type;
  memcpy(ev.pdata, pdata, size);
  int rv = s_fw.ops->send_event(&s_fw, &ev);
  free(ev.pdata);
  return rv;
}

static void
set_led(int on, int report)
{
  int was = (s_status & BLINKY_STATUS_LED_ON) ? 1 : 0;
  on      = on ? 1 : 0;
  gpio_put(BLINKY_LED_GPIO, on);
  if (on) {
    s_status |= BLINKY_STATUS_LED_ON;
  }
  else {
    s_status &= ~BLINKY_STATUS_LED_ON;
  }

  // CLASS1.INFORMATION ON/OFF when the LED is turned on/off from outside (not for each blink)
  if (report && (on != was)) {
    uint8_t data[3] = { 0, s_regs.zone, s_regs.subzone }; // index, zone, subzone
    send_event(VSCP_CLASS1_INFORMATION, on ? VSCP_TYPE_INFORMATION_ON : VSCP_TYPE_INFORMATION_OFF, data, 3);
  }
}

/*
  The BOOTSEL button: it is on the chip select of the flash, which is read by
  making the pin an input for a moment (the code must not run from flash
  then). See the pico-examples picoboard/button example.
*/
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void)
{
  const uint CS_PIN_INDEX = 1;
  uint32_t flags          = save_and_disable_interrupts();
  hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                  GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
  for (volatile int i = 0; i < 1000; ++i) {
  }
  bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
  hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                  GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                  IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
  restore_interrupts(flags);
  return pressed;
}

static int
button_pressed(void)
{
#if BLINKY_BUTTON_GPIO < 0
  return bootsel_pressed() ? 1 : 0;
#else
  return (0 == gpio_get(BLINKY_BUTTON_GPIO)) ? 1 : 0;
#endif
}

static void
blinky_init(void)
{
  set_led(0, 0);
  // Blink from start when the LED is enabled and an interval is set
  s_blinking   = (s_regs.control & BLINKY_CTRL_ENABLE_LED) && s_regs.blink_interval;
  s_last_blink = millis();
}

static void
button_changed(int pressed)
{
  if (pressed) {
    s_status |= BLINKY_STATUS_BTN_ON;
  }
  else {
    s_status &= ~BLINKY_STATUS_BTN_ON;
  }

  if (!(s_regs.control & BLINKY_CTRL_ENABLE_BTN)) {
    return;
  }

  printf("Button %s\n", pressed ? "pressed" : "released");

  // TURN-ON on press, TURN-OFF on release (control bit 3)
  // https://grodansparadis.github.io/vscp-doc-spec/#/./class1.control?id=type5
  if (s_regs.control & BLINKY_CTRL_ENABLE_BTN_TURNON) {
    uint8_t data[3] = { s_regs.button_byte0, s_regs.button_zone, s_regs.button_subzone };
    send_event(VSCP_CLASS1_CONTROL, pressed ? VSCP_TYPE_CONTROL_TURNON : VSCP_TYPE_CONTROL_TURNOFF, data, 3);
  }

  // START on press (bit 2), STOP on release (bit 1)
  // https://grodansparadis.github.io/vscp-doc-spec/#/./class1.information?id=type25
  if (pressed ? (s_regs.control & BLINKY_CTRL_ENABLE_BTN_START) : (s_regs.control & BLINKY_CTRL_ENABLE_BTN_STOP)) {
    uint8_t data[3] = { 0, s_regs.button_zone, s_regs.button_subzone };
    send_event(VSCP_CLASS1_INFORMATION, pressed ? VSCP_TYPE_INFORMATION_START : VSCP_TYPE_INFORMATION_STOP, data, 3);
  }
}

static void
blinky_work(void)
{
  // * * * Button (polled every BUTTON_POLL_MS and debounced) * * *
  static int stable = 0, last = 0, count = 0;
  static uint32_t last_poll = 0;
  if ((millis() - last_poll) >= BUTTON_POLL_MS) {
    last_poll   = millis();
    int pressed = button_pressed();
    if (pressed != last) {
      last  = pressed;
      count = 0;
    }
    else if ((pressed != stable) && (++count >= BUTTON_DEBOUNCE)) {
      stable = pressed;
      button_changed(pressed);
    }
  }

  // * * * Blink * * *
  if (s_blinking && (s_regs.control & BLINKY_CTRL_ENABLE_LED) && s_regs.blink_interval &&
      ((millis() - s_last_blink) >= s_regs.blink_interval)) {
    s_last_blink = millis();
    set_led(!(s_status & BLINKY_STATUS_LED_ON), 0);
  }

  // * * * Millisecond counter limit (alarm bit 7) * * *
  if (s_counter_wrapped) {
    s_counter_wrapped = 0;
    s_fw.alarm_status |= BLINKY_ALARM_COUNTER_LIMIT;
  }

  // * * * Save changed registers * * *
  if (s_save_pending && ((millis() - s_save_time) >= BLINKY_SAVE_DELAY_MS)) {
    regs_save();
  }
}

// Millisecond counter (timer interrupt, every millisecond)
static bool
counter_tick(repeating_timer_t *rt)
{
  if (s_regs.control & BLINKY_CTRL_ENABLE_MS_COUNTER) {
    if (0 == ++s_counter) {
      s_counter_wrapped = 1;
    }
  }
  return true;
}

// ****************************************************************************
//                         Level II firmware callbacks
// ****************************************************************************

static uint32_t
cb_get_milliseconds(vscp_frmw2_firmware_context_t *pctx)
{
  return millis();
}

static uint64_t
cb_get_timestamp(vscp_frmw2_firmware_context_t *pctx)
{
  return vscp_core_timestamp_ns();
}

// An event from the node: give it to every transport (they copy it)
static int
cb_send_event(vscp_frmw2_firmware_context_t *pctx, vscp_event_t *pev)
{
  if (NULL == pev) {
    return VSCP_ERROR_INVALID_POINTER;
  }
  link_server_send_event(pev);
  udp_transport_send_event(pev);
  mqtt_transport_send_event(pev);
  return VSCP_ERROR_SUCCESS;
}

static int
cb_dm_action(vscp_frmw2_firmware_context_t *pctx, const vscp_event_t *pev, uint8_t action, const uint8_t *pparam)
{
  const uint8_t param = (NULL != pparam) ? pparam[0] : 0;

  printf("Decision matrix action %d (parameter %d)\n", action, param);

  if (BLINKY_ACTION_CLEAR_COUNTER == action) {
    s_counter = 0;
    return VSCP_ERROR_SUCCESS;
  }

  // The LED actions work when the LED is enabled
  if (!(s_regs.control & BLINKY_CTRL_ENABLE_LED)) {
    return VSCP_ERROR_SUCCESS;
  }

  int on = (s_status & BLINKY_STATUS_LED_ON) ? 1 : 0;
  switch (action) {
    case BLINKY_ACTION_NOOP:
      break;
    case BLINKY_ACTION_LED_STATE:
      s_blinking = 0;
      set_led((BLINKY_ARG_LED_TOGGLE == param) ? !on : (BLINKY_ARG_LED_OFF != param), 1);
      break;
    case BLINKY_ACTION_LED_ON:
      s_blinking = 0;
      set_led(1, 1);
      break;
    case BLINKY_ACTION_LED_OFF:
      s_blinking = 0;
      set_led(0, 1);
      break;
    case BLINKY_ACTION_LED_TOGGLE:
      s_blinking = 0;
      set_led(!on, 1);
      break;
    case BLINKY_ACTION_START_BLINK:
      s_blinking   = (0 != s_regs.blink_interval); // Interval in registers 4-5
      s_last_blink = millis();
      break;
    case BLINKY_ACTION_STOP_BLINK:
      s_blinking = 0;
      break;
    default:
      return VSCP_ERROR_PARAMETER;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_segment_ctrl_heartbeat(vscp_frmw2_firmware_context_t *pctx, uint16_t segcrc, uint32_t time)
{
  return VSCP_ERROR_SUCCESS; // Not used on this node
}

static int
cb_report_events_of_interest(vscp_frmw2_firmware_context_t *pctx)
{
  return VSCP_ERROR_SUCCESS; // Interested in all events
}

static int
cb_set_event_time(vscp_frmw2_firmware_context_t *pctx, vscp_event_t *pev)
{
  if (NULL == pev) {
    return VSCP_ERROR_INVALID_POINTER;
  }
  pev->head |= VSCP_HEADER16_FRAME_VERSION_UNIX_NS;
  pev->timestamp_ns = vscp_core_timestamp_ns();
  return VSCP_ERROR_SUCCESS;
}

static int
cb_get_ip_addr(vscp_frmw2_firmware_context_t *pctx, uint8_t *ip, uint8_t size)
{
  if ((NULL == ip) || (size < 4)) {
    return VSCP_ERROR_INVALID_POINTER;
  }
  memcpy(ip, s_fw.ipaddr, (size < sizeof(s_fw.ipaddr)) ? size : sizeof(s_fw.ipaddr));
  return VSCP_ERROR_SUCCESS;
}

static int
cb_read_reg(vscp_frmw2_firmware_context_t *pctx, uint16_t page, uint32_t reg, uint8_t *pval)
{
  if (NULL == pval) {
    return VSCP_ERROR_INVALID_POINTER;
  }
  if (0 != page) { // All user registers are on page 0
    return VSCP_ERROR_PARAMETER;
  }

  uint32_t counter = s_counter;
  switch (reg) {
    case BLINKY_REG_ZONE:
      *pval = s_regs.zone;
      break;
    case BLINKY_REG_SUBZONE:
      *pval = s_regs.subzone;
      break;
    case BLINKY_REG_STATUS:
      *pval = s_status;
      break;
    case BLINKY_REG_CONTROL:
      *pval = s_regs.control;
      break;
    case BLINKY_REG_BLINK_INTERVAL_MSB:
      *pval = s_regs.blink_interval >> 8;
      break;
    case BLINKY_REG_BLINK_INTERVAL_LSB:
      *pval = s_regs.blink_interval & 0xff;
      break;
    case BLINKY_REG_COUNTER_0:
      *pval = counter >> 24;
      break;
    case BLINKY_REG_COUNTER_1:
      *pval = (counter >> 16) & 0xff;
      break;
    case BLINKY_REG_COUNTER_2:
      *pval = (counter >> 8) & 0xff;
      break;
    case BLINKY_REG_COUNTER_3:
      *pval = counter & 0xff;
      break;
    case BLINKY_REG_BUTTON_BYTE0:
      *pval = s_regs.button_byte0;
      break;
    case BLINKY_REG_BUTTON_ZONE:
      *pval = s_regs.button_zone;
      break;
    case BLINKY_REG_BUTTON_SUBZONE:
      *pval = s_regs.button_subzone;
      break;
    default:
      if ((reg >= BLINKY_REG_DM_START) && (reg < BLINKY_REG_DM_START + sizeof(s_regs.dm))) {
        *pval = s_regs.dm[reg - BLINKY_REG_DM_START];
        break;
      }
      return VSCP_ERROR_PARAMETER;
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_write_reg(vscp_frmw2_firmware_context_t *pctx, uint16_t page, uint32_t reg, uint8_t val)
{
  if (0 != page) { // All user registers are on page 0
    return VSCP_ERROR_PARAMETER;
  }

  switch (reg) {
    case BLINKY_REG_ZONE:
      s_regs.zone = pctx->zone = val; // The firmware uses its copy for the decision matrix
      break;
    case BLINKY_REG_SUBZONE:
      s_regs.subzone = pctx->subzone = val;
      break;
    case BLINKY_REG_STATUS:
      // Bit 1 turns the LED on/off (when the LED is enabled); the button bit is read only
      if (s_regs.control & BLINKY_CTRL_ENABLE_LED) {
        s_blinking = 0;
        set_led(val & BLINKY_STATUS_LED_ON, 1);
      }
      return VSCP_ERROR_SUCCESS; // Not saved
    case BLINKY_REG_CONTROL: {
      uint8_t old    = s_regs.control;
      s_regs.control = val;
      // Enabling the LED starts blinking (if an interval is set), disabling stops it
      if (!(old & BLINKY_CTRL_ENABLE_LED) && (val & BLINKY_CTRL_ENABLE_LED)) {
        s_blinking   = (0 != s_regs.blink_interval);
        s_last_blink = millis();
      }
      else if (!(val & BLINKY_CTRL_ENABLE_LED)) {
        s_blinking = 0;
      }
    } break;
    case BLINKY_REG_BLINK_INTERVAL_MSB:
      s_regs.blink_interval = (s_regs.blink_interval & 0x00ff) | ((uint16_t) val << 8);
      s_blinking            = (0 != s_regs.blink_interval); // Zero stops blinking
      break;
    case BLINKY_REG_BLINK_INTERVAL_LSB:
      s_regs.blink_interval = (s_regs.blink_interval & 0xff00) | val;
      s_blinking            = (0 != s_regs.blink_interval);
      break;
    case BLINKY_REG_COUNTER_0:
    case BLINKY_REG_COUNTER_1:
    case BLINKY_REG_COUNTER_2:
    case BLINKY_REG_COUNTER_3:
      s_counter = 0; // Writing any of them clears the counter
      return VSCP_ERROR_SUCCESS;
    case BLINKY_REG_BUTTON_BYTE0:
      s_regs.button_byte0 = val;
      break;
    case BLINKY_REG_BUTTON_ZONE:
      s_regs.button_zone = val;
      break;
    case BLINKY_REG_BUTTON_SUBZONE:
      s_regs.button_subzone = val;
      break;
    default:
      if ((reg >= BLINKY_REG_DM_START) && (reg < BLINKY_REG_DM_START + sizeof(s_regs.dm))) {
        s_regs.dm[reg - BLINKY_REG_DM_START] = val;
        break;
      }
      return VSCP_ERROR_PARAMETER;
  }
  regs_changed();
  return VSCP_ERROR_SUCCESS;
}

// The firmware keeps the user id and nickname in its context; save them
static int
cb_stdreg_change(vscp_frmw2_firmware_context_t *pctx, uint32_t stdreg)
{
  if (VSCP_STD_REGISTER_USER_ID == stdreg) {
    memcpy(s_regs.user_id, pctx->userId, sizeof(s_regs.user_id));
    regs_changed();
  }
  else if ((VSCP_STD_REGISTER_NICKNAME_ID_LSB == stdreg) || (VSCP_STD_REGISTER_NICKNAME_ID_MSB == stdreg)) {
    s_regs.nickname = pctx->nickname;
    regs_changed();
  }
  return VSCP_ERROR_SUCCESS;
}

static int
cb_restore_defaults(vscp_frmw2_firmware_context_t *pctx)
{
  regs_defaults();
  regs_to_context();
  s_counter = 0;
  blinky_init();
  regs_changed();
  return VSCP_ERROR_SUCCESS;
}

static void
cb_enter_bootloader(vscp_frmw2_firmware_context_t *pctx)
{
  // No bootloader on this node
}

static void
cb_reset(vscp_frmw2_firmware_context_t *pctx)
{
  vscp_core_request_restart();
}

static void
cb_feed_watchdog(vscp_frmw2_firmware_context_t *pctx)
{
}

static const vscp_frmw2_ops_t s_fw_ops = {
  .get_milliseconds          = cb_get_milliseconds,
  .get_timestamp             = cb_get_timestamp,
  .send_event                = cb_send_event,
  .dm_action                 = cb_dm_action,
  .segment_ctrl_heartbeat    = cb_segment_ctrl_heartbeat,
  .report_events_of_interest = cb_report_events_of_interest,
  .set_event_time            = cb_set_event_time,
  .get_ip_addr               = cb_get_ip_addr,
  .read_reg                  = cb_read_reg,
  .write_reg                 = cb_write_reg,
  .stdreg_change             = cb_stdreg_change,
  .restore_defaults          = cb_restore_defaults,
  .enter_bootloader          = cb_enter_bootloader,
  .reset                     = cb_reset,
  .feed_watchdog             = cb_feed_watchdog,
};

// ****************************************************************************
//                                 Main loop
// ****************************************************************************

void
vscp_core_work(void)
{
  vscp_event_t *pev = NULL;
  if (vscp_fifo_read(&s_queue, &pev)) {
    // Protocol events (register read/write, ...) and the decision matrix
    vscp_frmw2_work(&s_fw, pev);
    vscp_fwhlp_deleteEvent(&pev);
  }
  else {
    vscp_frmw2_work(&s_fw, NULL); // Heartbeat and capabilities
  }

  blinky_work();

  if (s_restart_requested) {
    if (s_save_pending) {
      regs_save();
    }
    printf("Restarting\n");
    sleep_ms(200); // Let replies go out
    watchdog_reboot(0, 0, 0);
    for (;;) {
    }
  }
}

void
vscp_core_init(const uint8_t *guid)
{
  regs_load();

  // LED
  gpio_init(BLINKY_LED_GPIO);
  gpio_set_dir(BLINKY_LED_GPIO, GPIO_OUT);

  // Button (pressed = low)
#if BLINKY_BUTTON_GPIO >= 0
  gpio_init(BLINKY_BUTTON_GPIO);
  gpio_set_dir(BLINKY_BUTTON_GPIO, GPIO_IN);
  gpio_pull_up(BLINKY_BUTTON_GPIO);
#endif

  // Millisecond counter
  add_repeating_timer_ms(-1, counter_tick, NULL, &s_counter_timer);

  // Level II firmware
  memset(&s_fw, 0, sizeof(s_fw));
  s_fw.level                      = VSCP_LEVEL2;
  s_fw.state                      = FRMW2_STATE_NONE;
  s_fw.bEnableErrorReporting      = 1;
  s_fw.bInterestedInAllEvents     = 1;
  s_fw.bDmLevel1Rows              = 1; // Level I (8 byte) decision matrix rows, as in the MDF
  s_fw.interval_heartbeat         = 60000;
  s_fw.interval_caps              = 60000;
  s_fw.pDm                        = s_regs.dm;
  s_fw.nDmRows                    = BLINKY_DM_ROWS;
  s_fw.sizeDmRow                  = BLINKY_DM_ROW_SIZE;
  s_fw.regOffsetDm                = BLINKY_REG_DM_START;
  s_fw.pageDm                     = 0;
  s_fw.high_end_srv_port          = 9598;
  s_fw.vscp_major_version         = 1;
  s_fw.vscp_minor_version         = 6;
  s_fw.firmware_major_version     = THIS_FIRMWARE_MAJOR_VERSION;
  s_fw.firmware_minor_version     = THIS_FIRMWARE_MINOR_VERSION;
  s_fw.firmware_sub_minor_version = THIS_FIRMWARE_RELEASE_VERSION;
  s_fw.firmware_device_code       = 0x0003;
  memcpy(s_fw.guid, guid, 16);
  strncpy((char *) s_fw.mdfurl, "eurosource.se/blinky0.json", sizeof(s_fw.mdfurl) - 1);
  strncpy((char *) s_fw.deviceName, THIS_FIRMWARE_DEVICE_NAME, sizeof(s_fw.deviceName) - 1);
  s_fw.ops = &s_fw_ops;
  regs_to_context();

  crcInit(); // Frame CRCs (UDP/multicast)

  blinky_init();

  vscp_fifo_init(&s_queue, CORE_QUEUE_SIZE);
  if (VSCP_ERROR_SUCCESS != vscp_frmw2_init(&s_fw)) {
    printf("Failed to start the VSCP firmware\n");
  }
}

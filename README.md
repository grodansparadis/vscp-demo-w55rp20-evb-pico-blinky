# vscp-demo-w55rp20-evb-pico-blinky

The [VSCP](https://www.vscp.org) Blinky demo node on the [WIZnet W55RP20-EVB-Pico](https://docs.wiznet.io/Product/Chip/MCU/W55RP20/w55rp20-evb-pico) (RP2040 with a W5500 Ethernet controller in one package). It has an LED that can be turned on and off or blink, a button that sends events, a millisecond counter and a decision matrix, as described in the [Blinky documentation](https://github.com/grodansparadis/vscp/wiki/Blinky-Documentation) and the [Blinky MDF](https://eurosource.se/blinky0.json).

The node can be reached in four ways at the same time:

| Transport | Default | VSCP |
| --- | --- | --- |
| VSCP link protocol (TCP/IP) | Port 9598, user `vscp`, password `secret`, up to 3 clients | [Text protocol](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_tcpiplink): login, `send`, `retr`, `rcvloop`, filters, ... |
| UDP | Port 33333, events sent to 255.255.255.255 (broadcast) | [VSCP frames](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_udp), one per datagram, not encrypted |
| Multicast | Group 224.0.23.158, port 44444 | [VSCP frames](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_multicast) on the VSCP multicast group |
| MQTT | Off; broker set in the configuration | [VSCP JSON events](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_over_mqtt), published on `vscp/{guid}/{class}/{type}`, received on `vscp/{guid}/in` |

Events received over any of them are handled by the node (register read/write and the other protocol events) and run through the decision matrix. Events from the node are sent over all of them.

## Hardware

The W55RP20-EVB-Pico has the Pico form factor. The W5500 is connected to the RP2040 inside the W55RP20 on GPIO20-25 (PIO SPI); these pins are not on the header.

| | |
| --- | --- |
| LED | The user LED on GPIO19 |
| Button | The BOOTSEL button. A button on a GPIO (pressed = low) can be used instead: `-DBLINKY_BUTTON_GPIO=15` |
| Ethernet | 10/100 Mbit, DHCP or a static address |
| Log | USB serial (115200) |

The node also works without a network (LED, button, counter, saved registers). The network is started when the Ethernet link comes up, and the log says so if the W5500 does not answer (for example when the firmware is put on another board).

The BOOTSEL button is read the way the [pico-examples](https://github.com/raspberrypi/pico-examples/tree/master/picoboard/button) do: the flash chip select is made an input for a few microseconds.

## Build and flash

Install the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x and an ARM GCC toolchain (`gcc-arm-none-eabi`). The vscp-firmware code, [WIZnet-PICO-C](https://github.com/WIZnet-ioNIC/WIZnet-PICO-C) (ioLibrary and the W55RP20 port) and cJSON are fetched when the project is configured.

```bash
git clone https://github.com/grodansparadis/vscp-demo-w55rp20-evb-pico-blinky.git
cd vscp-demo-w55rp20-evb-pico-blinky
cp blinky_local.cmake.example blinky_local.cmake   # optional: static IP, MQTT broker ...
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j4
```

Hold BOOTSEL while connecting the USB cable and copy `build/vscp-blinky.uf2` to the RPI-RP2 drive, or use `picotool load -x build/vscp-blinky.uf2 -f`.

All settings are CMake cache variables, set with `-D<name>=<value>` or in `blinky_local.cmake` (not committed):

| Setting | Default | |
| --- | --- | --- |
| `BLINKY_IP` | empty | Static IPv4 address; empty = DHCP. Also `BLINKY_NETMASK`, `BLINKY_GATEWAY` |
| `BLINKY_LED_GPIO` | 19 | |
| `BLINKY_BUTTON_GPIO` | -1 | -1 = the BOOTSEL button |
| `BLINKY_LINK_PORT` | 9598 | 0 = no link protocol server |
| `BLINKY_LINK_MAX_CLIENTS` | 3 | 1-3 |
| `BLINKY_LINK_USER`, `BLINKY_LINK_PASSWORD` | vscp, secret | |
| `BLINKY_UDP_PORT` | 33333 | 0 = off |
| `BLINKY_UDP_REMOTE_HOST` | 255.255.255.255 | Events are sent here |
| `BLINKY_MULTICAST_GROUP` | 224.0.23.158 | Empty = off. Also `BLINKY_MULTICAST_PORT` (44444), `BLINKY_MULTICAST_TTL` (1) |
| `BLINKY_MQTT_BROKER` | empty | Broker IPv4 address; empty = no MQTT. Also `BLINKY_MQTT_PORT`, `_USER`, `_PASSWORD`, `_PUBLISH_TOPIC`, `_SUBSCRIBE_TOPIC` |
| `BLINKY_SNTP_SERVER` | 216.239.35.0 | SNTP server IPv4 address (time.google.com); empty = off |

After a change in `blinky_local.cmake`, delete the build directory (or `build/CMakeCache.txt`) so the new values are used.

The USB serial port shows what the node does:

```
VSCP Blinky demo for the W55RP20-EVB-Pico 0.1.0
MAC 02:66:1A:2B:3C:4D
DHCP: asking for an address
IP address 192.168.1.60
VSCP link protocol server on port 9598 (3 clients)
UDP: port 33333, events to 255.255.255.255
Multicast: group 224.0.23.158 port 44444
SNTP: 2026-11-20T16:30:00Z
```

## The node

### GUID

The W5500 has no MAC address of its own. A locally administered address is made from the unique id of the flash (`02:xx:xx:xx:xx:xx`), and the GUID from that: `FF:FF:FF:FF:FF:FF:FF:FE:<MAC>:00:00` ([Ethernet/MAC address based GUID](https://grodansparadis.github.io/vscp-doc-spec/#/./vscp_globally_unique_identifiers)). Get it with `ggid` over the link protocol.

### Registers

| Register (page 0) | Use |
| --- | --- |
| 0, 1 | Zone and subzone of the node [saved] |
| 2 | Status: bit 1 LED on, bit 0 button pressed. Writing bit 1 turns the LED on/off (when the LED is enabled) |
| 3 | Control [saved, default 0xCE]: bit 7 LED enabled, bit 6 button enabled, bit 3 send TURN-ON on press and TURN-OFF on release, bit 2 send START on press, bit 1 send STOP on release, bit 0 millisecond counter running |
| 4, 5 | Blink interval in ms, MSB first [saved, default 500]. Zero stops blinking |
| 6-9 | Millisecond counter, MSB first. Writing any of them clears it. When it wraps, alarm bit 7 is set |
| 10-12 | Byte 0, zone and subzone of the button events [saved] |
| 0x60-0x7F | Decision matrix: four Level I rows of eight bytes (origin, flags, class mask, class filter, type mask, type filter, action, parameter) [saved] |

The standard registers are at 0xFFFFFF80 (Level II). The node is a Level II node, but it uses Level I decision matrix rows, as in the MDF, and it also answers CLASS1.PROTOCOL register read/write for nickname 0x10 (the default nickname, saved when it is changed).

Saved registers are written to the last sector of the flash one second after the last change, so a burst of writes is one save.

### Decision matrix actions

| Action | |
| --- | --- |
| 0 | No operation |
| 1 | LED off/on/toggle (parameter 0/1/2) |
| 2 | LED on |
| 3 | LED off |
| 4 | Toggle the LED |
| 5 | Start blinking (interval in registers 4-5) |
| 6 | Stop blinking |
| 7 | Clear the millisecond counter |

The LED actions work when control bit 7 is set. On, off and toggle stop blinking.

### Decision matrix actions

| Action | |
| --- | --- |
| 0 | No operation |
| 1 | LED off/on/toggle (parameter 0/1/2) |
| 2 | LED on |
| 3 | LED off |
| 4 | Toggle the LED |
| 5 | Start blinking (interval in registers 4-5) |
| 6 | Stop blinking |
| 7 | Clear the millisecond counter |

The LED actions work when control bit 7 is set. On, off and toggle stop blinking.

### Events from the node

- CLASS1.INFORMATION ON/OFF (20/3, 20/4) when the LED is turned on or off by an action or a status register write (not for each blink). Data: index 0, zone, subzone
- CLASS1.CONTROL TURN-ON (30/5) when the button is pressed and TURN-OFF (30/6) when it is released (control bit 3). Data: registers 10-12
- CLASS1.INFORMATION START (20/25) on press and STOP (20/24) on release (control bits 2 and 1). Data: 0, button zone, button subzone
- CLASS2.INFORMATION node heartbeat (1026/2) and CLASS2.PROTOCOL capabilities (1024/20) every minute
- Responses to the protocol events (register read/write, who is there, ...)

The time in the events is the Unix time in nanoseconds from SNTP, or the time since the start until the time is set.


## Try it

### VSCP link protocol

```
$ telnet 192.168.1.60 9598
Welcome to the VSCP W55RP20-EVB-Pico Blinky demo node
...
+OK
user vscp
+OK - User name accepted, password please
pass secret
+OK - Ready to work.
send 0,0,9,0,,0,-,0x10,3
+OK - Success.
retr
0,0,10,0,...,FF:FF:FF:FF:FF:FF:FF:FE:02:66:1A:2B:3C:4D:00:00,3,206
+OK - Success.
```

`send 0,0,9,...,0x10,3` reads register 3 (control) of node 0x10; the answer is CLASS1.PROTOCOL read/write response (0/10) with the register and its value (206 = 0xCE). `rcvloop` sends every event from the node as it comes. [VSCP Works](https://github.com/grodansparadis/vscp-works-qt) can be used with a tcp/ip connection to the node, and the MDF lets it show and set the registers by name.

### MQTT

Set the broker (`blinky_local.cmake`):

```cmake
set(BLINKY_MQTT_BROKER "192.168.1.9" CACHE STRING "")
```

Each event is published as one line of JSON, as made by the vscp-firmware helper. Events sent to the node can use those keys (`class`, `type`, `data`, ...) or the `vscpClass`, `vscpType`, `vscpData`, ... keys.

```bash
mosquitto_sub -h 192.168.1.9 -t 'vscp/#' -v
mosquitto_pub -h 192.168.1.9 -t 'vscp/FF:FF:FF:FF:FF:FF:FF:FE:02:66:1A:2B:3C:4D:00:00/in' \
  -m '{"vscpHead":0,"vscpClass":0,"vscpType":9,"vscpData":[16,3]}'   # read register 3
```

When the broker can not be reached the node keeps working and tries again every five seconds.

### Test script

[tools/blinky_test.py](tools/blinky_test.py) tests a node over each transport (only the Python standard library; MQTT uses `mosquitto_pub`/`mosquitto_sub`):

```bash
tools/blinky_test.py --host 192.168.1.60 link
tools/blinky_test.py --host 192.168.1.60 udp
tools/blinky_test.py multicast
tools/blinky_test.py --host 192.168.1.60 --broker 192.168.1.9 mqtt
tools/blinky_test.py --host 192.168.1.60 --broker 192.168.1.9 all
```

## Layout

| | |
| --- | --- |
| `main/main.c` | MAC address and GUID, the main loop |
| `main/network.c` | W5500 start, DHCP, SNTP, millisecond timer |
| `main/blinky.c` | The node: Level II firmware callbacks, registers, LED, button, counter, decision matrix actions |
| `main/storage.c` | Saved registers in the last flash sector |
| `main/link_server.c` | VSCP link protocol server |
| `main/udp_transport.c` | UDP and multicast |
| `main/mqtt_transport.c` | MQTT |
| `CMakeLists.txt` | Settings and dependencies |
| `tools/blinky_test.py` | Test script |

Everything runs in the main loop (no RTOS). The W5500 has eight sockets: 0 DHCP, 1 SNTP, 2-4 link protocol clients, 5 UDP, 6 multicast, 7 MQTT.

The VSCP code is from [vscp-firmware](https://github.com/grodansparadis/vscp-firmware) (helper, link protocol and Level II firmware), fetched by CMake at the commit in [CMakeLists.txt](CMakeLists.txt). Use `-DFETCHCONTENT_SOURCE_DIR_VSCP_FIRMWARE=<path>` to build with a local checkout.

## Other Blinky demos

See the [Blinky page](https://github.com/grodansparadis/vscp/wiki/Blinky) for the other implementations, for example [vscp-demo-esp32-wifi-blinky](https://github.com/grodansparadis/vscp-demo-esp32-wifi-blinky) and [vscp-demo-link-stm32f401-wiz-ip20-blinky](https://github.com/grodansparadis/vscp-demo-link-stm32f401-wiz-ip20-blinky).

## License

MIT, see [LICENSE](LICENSE).

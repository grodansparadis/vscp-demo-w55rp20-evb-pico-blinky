# Changes in vscp-demo-w55rp20-evb-pico-blinky

Every change that goes into a release is listed here, newest release first.
Add each change under **Unreleased** when it is made. When a release is
tagged, rename **Unreleased** to the version and date, and start a new
**Unreleased** section above it.

## Unreleased

- First version: the VSCP Blinky node (LED, button, millisecond counter, four row decision matrix, registers saved in flash) on the WIZnet W55RP20-EVB-Pico (RP2040 + W5500 Ethernet)
- Transports, all at the same time: VSCP link protocol server (TCP/IP, three clients), VSCP frames over UDP and on the VSCP multicast group, VSCP JSON events over MQTT
- DHCP or a static address, MAC address and GUID from the unique id of the flash, event time from SNTP
- The button is the BOOTSEL button, or a button on any GPIO
- The node works without an Ethernet link or W5500 (the WIZnet start-up code waits for a link for ever); the network starts when the link comes up
- `tools/blinky_test.py` tests a node over each transport

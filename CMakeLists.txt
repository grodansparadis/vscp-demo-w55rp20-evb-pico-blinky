# VSCP Blinky demo for the WIZnet W55RP20-EVB-Pico (RP2040 + W5500 Ethernet)
cmake_minimum_required(VERSION 3.13)

# Local settings that are not committed (static IP, MQTT broker, ...)
include(${CMAKE_CURRENT_LIST_DIR}/blinky_local.cmake OPTIONAL)

# The W55RP20-EVB-Pico has no board file in the Pico SDK; it is a Pico
# (RP2040, 2 MB flash) with the W5500 on GPIO20-25 and the LED on GPIO19.
set(PICO_BOARD pico CACHE STRING "Board type")

# Pico SDK: PICO_SDK_PATH, or -DPICO_SDK_FETCH_FROM_GIT=ON
include(pico_sdk_import.cmake)

project(vscp-blinky C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# ---------------------------------------------------------------------------
#                                Settings
# Set with -D<name>=<value> or in blinky_local.cmake
# ---------------------------------------------------------------------------

set(BLINKY_IP "" CACHE STRING "Static IPv4 address (empty = DHCP)")
set(BLINKY_NETMASK "255.255.255.0" CACHE STRING "Netmask (static IP)")
set(BLINKY_GATEWAY "" CACHE STRING "Gateway (static IP)")
set(BLINKY_LED_GPIO 19 CACHE STRING "LED GPIO")
set(BLINKY_BUTTON_GPIO -1 CACHE STRING "Button GPIO (pressed = low), -1 = the BOOTSEL button")

set(BLINKY_LINK_PORT 9598 CACHE STRING "VSCP link protocol port (0 = off)")
set(BLINKY_LINK_MAX_CLIENTS 3 CACHE STRING "Link protocol clients at the same time (1-3)")
set(BLINKY_LINK_USER "vscp" CACHE STRING "Link protocol user")
set(BLINKY_LINK_PASSWORD "secret" CACHE STRING "Link protocol password")

set(BLINKY_UDP_PORT 33333 CACHE STRING "UDP port (0 = off)")
set(BLINKY_UDP_REMOTE_HOST "255.255.255.255" CACHE STRING "Events are sent to this IPv4 address")

set(BLINKY_MULTICAST_GROUP "224.0.23.158" CACHE STRING "Multicast group (empty = off)")
set(BLINKY_MULTICAST_PORT 44444 CACHE STRING "Multicast port")
set(BLINKY_MULTICAST_TTL 1 CACHE STRING "Multicast TTL")

set(BLINKY_MQTT_BROKER "" CACHE STRING "MQTT broker IPv4 address (empty = MQTT off)")
set(BLINKY_MQTT_PORT 1883 CACHE STRING "MQTT broker port")
set(BLINKY_MQTT_USER "" CACHE STRING "MQTT user")
set(BLINKY_MQTT_PASSWORD "" CACHE STRING "MQTT password")
set(BLINKY_MQTT_PUBLISH_TOPIC "vscp/{guid}/{class}/{type}" CACHE STRING "MQTT publish topic")
set(BLINKY_MQTT_SUBSCRIBE_TOPIC "vscp/{guid}/in" CACHE STRING "MQTT subscribe topic")

set(BLINKY_SNTP_SERVER "216.239.35.0" CACHE STRING "SNTP server IPv4 address (empty = off)")

# ---------------------------------------------------------------------------
#                              Dependencies
# ---------------------------------------------------------------------------

include(FetchContent)

# vscp-firmware. Use -DFETCHCONTENT_SOURCE_DIR_VSCP_FIRMWARE=<path> for a local checkout.
FetchContent_Declare(
    vscp_firmware
    URL https://codeload.github.com/grodansparadis/vscp-firmware/tar.gz/f8bc40d0585df638ef7f742ff25106920acc3277
    URL_HASH SHA256=d33c3c64f02b4877fd9753cad2834d206028cc3ebdd2afa00621dd21d85de94b
    SOURCE_SUBDIR __sources_only__
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# WIZnet-PICO-C (W55RP20 PIO SPI port) with its ioLibrary_Driver submodule
FetchContent_Declare(
    wiznet_pico_c
    GIT_REPOSITORY https://github.com/WIZnet-ioNIC/WIZnet-PICO-C.git
    GIT_TAG 2f494fc57a0e950aa80749470ffe201206043cfe
    GIT_SUBMODULES "libraries/ioLibrary_Driver"
    SOURCE_SUBDIR __sources_only__
)

# cJSON (VSCP JSON events over MQTT)
FetchContent_Declare(
    cjson
    URL https://codeload.github.com/DaveGamble/cJSON/tar.gz/b2890c8d76bbb64e710585ebc0a917196b9c67e7
    URL_HASH SHA256=c3866048d822428ab289d20842c75a39c92c26e967fed2f0706f6f15e6894082
    SOURCE_SUBDIR __sources_only__
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(vscp_firmware wiznet_pico_c cjson)

# WIZnet libraries for the W55RP20 (W5500 core)
set(BOARD_NAME W55RP20_EVB_PICO)
set(WIZNET_CHIP W5500)
add_definitions(-D_WIZCHIP_=W5500 -DDEVICE_BOARD_NAME=W55RP20_EVB_PICO -D_WIZCHIP_SPI_SCLK_SPEED=30)
set(WIZNET_DIR ${wiznet_pico_c_SOURCE_DIR}/libraries/ioLibrary_Driver)
set(PORT_DIR ${wiznet_pico_c_SOURCE_DIR}/port)
add_subdirectory(${wiznet_pico_c_SOURCE_DIR}/libraries ${CMAKE_BINARY_DIR}/wiznet-libraries)
add_subdirectory(${PORT_DIR} ${CMAKE_BINARY_DIR}/wiznet-port)

# ---------------------------------------------------------------------------
#                                Firmware
# ---------------------------------------------------------------------------

set(VSCP_SRC ${vscp_firmware_SOURCE_DIR}/src)

add_executable(vscp-blinky
    main/main.c
    main/network.c
    main/blinky.c
    main/storage.c
    main/link_server.c
    main/udp_transport.c
    main/mqtt_transport.c
    ${VSCP_SRC}/vscp-fifo.c
    ${VSCP_SRC}/vscp-crc.c
    ${VSCP_SRC}/vscp-firmware-helper.c
    ${VSCP_SRC}/vscp-firmware-level2.c
    ${VSCP_SRC}/vscp-link-protocol.c
    ${cjson_SOURCE_DIR}/cJSON.c
)

target_include_directories(vscp-blinky PRIVATE main ${VSCP_SRC} ${cjson_SOURCE_DIR})

target_compile_options(vscp-blinky PRIVATE -Wall)
set_source_files_properties(
    ${VSCP_SRC}/vscp-fifo.c ${VSCP_SRC}/vscp-crc.c ${VSCP_SRC}/vscp-firmware-helper.c
    ${VSCP_SRC}/vscp-firmware-level2.c ${VSCP_SRC}/vscp-link-protocol.c ${cjson_SOURCE_DIR}/cJSON.c
    PROPERTIES COMPILE_OPTIONS "-w")

target_compile_definitions(vscp-blinky PRIVATE
    PICO_STACK_SIZE=0x1000          # The link protocol has line buffers on the stack
    BLINKY_IP="${BLINKY_IP}"
    BLINKY_NETMASK="${BLINKY_NETMASK}"
    BLINKY_GATEWAY="${BLINKY_GATEWAY}"
    BLINKY_LED_GPIO=${BLINKY_LED_GPIO}
    BLINKY_BUTTON_GPIO=${BLINKY_BUTTON_GPIO}
    BLINKY_LINK_PORT=${BLINKY_LINK_PORT}
    BLINKY_LINK_MAX_CLIENTS=${BLINKY_LINK_MAX_CLIENTS}
    BLINKY_LINK_USER="${BLINKY_LINK_USER}"
    BLINKY_LINK_PASSWORD="${BLINKY_LINK_PASSWORD}"
    BLINKY_UDP_PORT=${BLINKY_UDP_PORT}
    BLINKY_UDP_REMOTE_HOST="${BLINKY_UDP_REMOTE_HOST}"
    BLINKY_MULTICAST_GROUP="${BLINKY_MULTICAST_GROUP}"
    BLINKY_MULTICAST_PORT=${BLINKY_MULTICAST_PORT}
    BLINKY_MULTICAST_TTL=${BLINKY_MULTICAST_TTL}
    BLINKY_MQTT_BROKER="${BLINKY_MQTT_BROKER}"
    BLINKY_MQTT_PORT=${BLINKY_MQTT_PORT}
    BLINKY_MQTT_USER="${BLINKY_MQTT_USER}"
    BLINKY_MQTT_PASSWORD="${BLINKY_MQTT_PASSWORD}"
    BLINKY_MQTT_PUBLISH_TOPIC="${BLINKY_MQTT_PUBLISH_TOPIC}"
    BLINKY_MQTT_SUBSCRIBE_TOPIC="${BLINKY_MQTT_SUBSCRIBE_TOPIC}"
    BLINKY_SNTP_SERVER="${BLINKY_SNTP_SERVER}"
)

target_link_libraries(vscp-blinky PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_flash
    hardware_watchdog
    hardware_clocks
    ETHERNET_FILES
    IOLIBRARY_FILES
    TIMER_FILES
    DHCP_FILES
    MQTT_FILES
    SNTP_FILES
)

pico_enable_stdio_usb(vscp-blinky 1)
pico_enable_stdio_uart(vscp-blinky 0)
pico_add_extra_outputs(vscp-blinky)

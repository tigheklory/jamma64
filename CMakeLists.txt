cmake_minimum_required(VERSION 3.13)

# Use the same lwIP config style as the official httpd example
set(PICO_LWIP_CONFIG_FILE ${CMAKE_CURRENT_LIST_DIR}/lwipopts.h)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(jamma64 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Build toggles for timing-focused validation.
option(JAMMA64_ENABLE_WIFI "Enable Wi-Fi/web stack" ON)
option(JAMMA64_ENABLE_N64_DIAG "Enable verbose N64 protocol diagnostics" OFF)
option(JAMMA64_ENABLE_STATUS_PRINTS "Enable 1 Hz status prints" OFF)
option(JAMMA64_ENABLE_USB_DEBUG "Enable USB CDC/stdout debug prints" OFF)
set(JAMMA64_RX_TO_TX_DELAY_US "0" CACHE STRING "N64 RX->TX turnaround delay in microseconds")
set(JAMMA64_N64_BACKEND "builtin" CACHE STRING "N64 backend: builtin or joybus")
set_property(CACHE JAMMA64_N64_BACKEND PROPERTY STRINGS builtin joybus)
set(JAMMA64_JOYBUS_PIO_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/joybus-pio" CACHE PATH "Path to a local joybus-pio checkout")

if(JAMMA64_ENABLE_WIFI)
  set(JAMMA64_ENABLE_WIFI_DEF 1)
else()
  set(JAMMA64_ENABLE_WIFI_DEF 0)
endif()

if(JAMMA64_ENABLE_N64_DIAG)
  set(JAMMA64_ENABLE_N64_DIAG_DEF 1)
else()
  set(JAMMA64_ENABLE_N64_DIAG_DEF 0)
endif()

if(JAMMA64_ENABLE_STATUS_PRINTS)
  set(JAMMA64_ENABLE_STATUS_PRINTS_DEF 1)
else()
  set(JAMMA64_ENABLE_STATUS_PRINTS_DEF 0)
endif()

if(JAMMA64_ENABLE_USB_DEBUG)
  set(JAMMA64_ENABLE_USB_DEBUG_DEF 1)
else()
  set(JAMMA64_ENABLE_USB_DEBUG_DEF 0)
endif()

# Firmware version tag for boot/log identification.
# Format example: g1a2b3c4 (fallback: dev)
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} rev-parse --short=8 HEAD
  OUTPUT_VARIABLE JAMMA64_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT JAMMA64_GIT_HASH)
  set(JAMMA64_GIT_HASH "dev")
endif()

execute_process(
  COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} status --porcelain
  OUTPUT_VARIABLE JAMMA64_GIT_DIRTY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(JAMMA64_GIT_DIRTY)
  set(JAMMA64_FW_VERSION_STR "g${JAMMA64_GIT_HASH}-dirty")
else()
  set(JAMMA64_FW_VERSION_STR "g${JAMMA64_GIT_HASH}")
endif()
string(TIMESTAMP JAMMA64_BUILD_DATE_STR "%Y-%m-%dT%H:%M:%SZ" UTC)

pico_sdk_init()

# --- Build the HTTPD content into pico_fsdata.inc ---
pico_add_library(pico_httpd_content NOFLAG)
pico_set_lwip_httpd_content(pico_httpd_content INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/content/404.html
  ${CMAKE_CURRENT_LIST_DIR}/content/api.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/index.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/wiring.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/img/favicon.ico
  ${CMAKE_CURRENT_LIST_DIR}/content/img/jamma64-itouch-icon.png
  ${CMAKE_CURRENT_LIST_DIR}/content/img/rpi.png.gz
  ${CMAKE_CURRENT_LIST_DIR}/content/img/pico_2w_pinout_text_labels.svg
  ${CMAKE_CURRENT_LIST_DIR}/content/img/capcom_2p_layout_lineart_hover.svg
  ${CMAKE_CURRENT_LIST_DIR}/content/img/capcom_2p_layout_lineart_hover.svgz
  ${CMAKE_CURRENT_LIST_DIR}/content/img/controller_clickable_jamma64_v8.svgz
)

add_executable(jamma64
  src/main.c
  src/web.c
  src/inputs.c
  src/profile.c
  src/mapping_store.c
  src/wifi_config.c
  src/n64_virtual.c
  src/usb_msc.c
  src/usb_descriptors.c
)

if(JAMMA64_N64_BACKEND STREQUAL "builtin")
  target_sources(jamma64 PRIVATE src/n64.c)
  pico_generate_pio_header(jamma64 ${CMAKE_CURRENT_LIST_DIR}/src/n64_io.pio)
  target_compile_definitions(jamma64 PRIVATE JAMMA64_N64_BACKEND_BUILTIN=1)
elseif(JAMMA64_N64_BACKEND STREQUAL "joybus")
  if(NOT EXISTS "${JAMMA64_JOYBUS_PIO_DIR}")
    message(FATAL_ERROR "JAMMA64_N64_BACKEND=joybus but JAMMA64_JOYBUS_PIO_DIR does not exist: ${JAMMA64_JOYBUS_PIO_DIR}")
  endif()
  if(NOT EXISTS "${JAMMA64_JOYBUS_PIO_DIR}/include/N64Console.hpp")
    message(FATAL_ERROR "Expected joybus-pio header not found: ${JAMMA64_JOYBUS_PIO_DIR}/include/N64Console.hpp")
  endif()
  if(NOT EXISTS "${JAMMA64_JOYBUS_PIO_DIR}/src/joybus.c")
    message(FATAL_ERROR "Expected joybus-pio source not found: ${JAMMA64_JOYBUS_PIO_DIR}/src/joybus.c")
  endif()
  if(NOT EXISTS "${JAMMA64_JOYBUS_PIO_DIR}/src/N64Console.cpp")
    message(FATAL_ERROR "Expected joybus-pio source not found: ${JAMMA64_JOYBUS_PIO_DIR}/src/N64Console.cpp")
  endif()
  pico_generate_pio_header(jamma64 ${JAMMA64_JOYBUS_PIO_DIR}/src/joybus.pio)
  target_sources(jamma64 PRIVATE
    src/n64_joybus.cpp
    ${JAMMA64_JOYBUS_PIO_DIR}/src/joybus.c
    ${JAMMA64_JOYBUS_PIO_DIR}/src/N64Console.cpp
  )
  target_include_directories(jamma64 PRIVATE "${JAMMA64_JOYBUS_PIO_DIR}/include")
  target_link_libraries(jamma64 hardware_pio)
  target_compile_definitions(jamma64 PRIVATE JAMMA64_N64_BACKEND_JOYBUS=1)
else()
  message(FATAL_ERROR "Invalid JAMMA64_N64_BACKEND='${JAMMA64_N64_BACKEND}'. Use builtin or joybus.")
endif()

target_compile_definitions(jamma64 PRIVATE
  CFG_TUSB_CONFIG_FILE=\"tusb_config.h\"
  CFG_TUD_ENABLED=1
  CFG_TUD_CDC=1
  CFG_TUD_MSC=1
  JAMMA64_FW_VERSION=\"${JAMMA64_FW_VERSION_STR}\"
  JAMMA64_BUILD_DATE=\"${JAMMA64_BUILD_DATE_STR}\"
  JAMMA64_ENABLE_WIFI=${JAMMA64_ENABLE_WIFI_DEF}
  JAMMA64_ENABLE_N64_DIAG=${JAMMA64_ENABLE_N64_DIAG_DEF}
  JAMMA64_ENABLE_STATUS_PRINTS=${JAMMA64_ENABLE_STATUS_PRINTS_DEF}
  JAMMA64_ENABLE_USB_DEBUG=${JAMMA64_ENABLE_USB_DEBUG_DEF}
  JAMMA64_RX_TO_TX_DELAY_US=${JAMMA64_RX_TO_TX_DELAY_US}
)


target_include_directories(jamma64 PRIVATE
  src
  ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(jamma64
  pico_stdlib
  pico_cyw43_arch_lwip_threadsafe_background
  pico_lwip_http
  pico_httpd_content
  hardware_sync
  hardware_watchdog
  tinyusb_device
  tinyusb_board
  hardware_watchdog
)

pico_enable_stdio_usb(jamma64 ${JAMMA64_ENABLE_USB_DEBUG_DEF})
pico_enable_stdio_uart(jamma64 0)

pico_add_extra_outputs(jamma64)

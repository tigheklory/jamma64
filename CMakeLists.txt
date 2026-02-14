cmake_minimum_required(VERSION 3.13)

# Use the same lwIP config style as the official httpd example
set(PICO_LWIP_CONFIG_FILE ${CMAKE_CURRENT_LIST_DIR}/lwipopts.h)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(jamma64 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# --- Build the HTTPD content into pico_fsdata.inc ---
pico_add_library(pico_httpd_content NOFLAG)
pico_set_lwip_httpd_content(pico_httpd_content INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/content/404.html
  ${CMAKE_CURRENT_LIST_DIR}/content/index.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/test.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/ledpass.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/ledfail.shtml
  ${CMAKE_CURRENT_LIST_DIR}/content/img/rpi.png.gz
)

add_executable(jamma64
  src/main.c
  src/n64.c
  src/web.c
  src/inputs.c
  src/profile.c
  src/event_log.c
  src/wifi_config.c
  src/usb_msc.c
  src/usb_descriptors.c
)

pico_generate_pio_header(jamma64 ${CMAKE_CURRENT_LIST_DIR}/src/n64_tx.pio)

target_compile_definitions(jamma64 PRIVATE
  CFG_TUSB_CONFIG_FILE=\"tusb_config.h\"
  CFG_TUD_ENABLED=1
  CFG_TUD_CDC=1
  CFG_TUD_MSC=1
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

pico_enable_stdio_usb(jamma64 0)
pico_enable_stdio_uart(jamma64 0)

pico_add_extra_outputs(jamma64)

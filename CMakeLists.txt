# Top-level CMake for the ESP-IDF port of esp32-s3-box-basic.
#
# This is a parallel project to the arduino-cli build at
# c:\dev\repos\esp32-s3-box-basic\. Both share the same TI_* submodule
# source via the components/ directory below. The arduino-cli build
# stays as a working reference while this IDF port is brought up.
#
# Build:  idf.py build
# Flash:  idf.py -p COM19 flash
# Monitor: idf.py monitor
cmake_minimum_required(VERSION 3.16)

# Pull our local copies of the Arduino-style libraries via components/.
set(EXTRA_COMPONENT_DIRS components)

# Match the arduino-cli FQBN's `CDCOnBoot=cdc`. arduino-esp32's
# HardwareSerial.h reroutes `Serial` to:
#   HWCDCSerial — when ARDUINO_USB_CDC_ON_BOOT=1 AND ARDUINO_USB_MODE=1
#   USBSerial   — when ARDUINO_USB_CDC_ON_BOOT=1 AND ARDUINO_USB_MODE=0 (TinyUSB)
#   Serial0     — when ARDUINO_USB_CDC_ON_BOOT=0 (UART0)
# Box-3 has no UART bridge and we don't ship TinyUSB, so we want the
# HW CDC mode — same USB-Serial-JTAG endpoint that the ESP-IDF console
# (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y) writes to.
add_compile_definitions(
    ARDUINO_USB_CDC_ON_BOOT=1
    ARDUINO_USB_MODE=1
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-s3-box-basic)

# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS 

  "./components/audio"

  "./components/backlight"
  "./components/battery"

  "./components/certs"

  "./components/config"

  "./components/display/lv_port"
  "./components/display/lv_v9.3"
  "./components/display/ST7789"

  "./components/decode/png"
  "./components/decode/jpg"
  "./components/decode"

  "./components/error"

  "./components/fs"

  "./components/I2C"

  "./components/msg"

  "./components/net"
  "./components/notify"

  "./components/profile"

  "./components/sensor"

  "./components/settings"

  "./components/tx"

  "./components/ui"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BeaconOps)

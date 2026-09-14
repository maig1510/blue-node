# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Goes into the app descriptor, which node_protocol reports to the backend as
# the "fw" field of the online state message (and lands in devices.firmware).
# Without it ESP-IDF falls back to `git describe`, i.e. a commit hash.
# Must be set before project().
set(PROJECT_VER "0.0.1")

project(esp32)

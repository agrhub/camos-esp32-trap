# SPDX-License-Identifier: AGPL-3.0-only
# FlyBot Sentinel: CamOS device protocol v1 subset on ESP32 camera boards (no AI on the device).
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.6.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camos-esp32)

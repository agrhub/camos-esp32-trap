# SPDX-License-Identifier: AGPL-3.0-only
# Build-time defaults (CONTRACTS 2a): defaults.local.json (git-ignored, your bench Wi-Fi) when present, else the
# public defaults.example.json, turned into camos_defaults.h. No secrets go through here.
set(CAMOS_DEFAULTS_JSON "${CMAKE_CURRENT_LIST_DIR}/../defaults.local.json")
if(NOT EXISTS "${CAMOS_DEFAULTS_JSON}")
    set(CAMOS_DEFAULTS_JSON "${CMAKE_CURRENT_LIST_DIR}/../defaults.example.json")
endif()
get_filename_component(CAMOS_DEFAULTS_NAME "${CAMOS_DEFAULTS_JSON}" NAME)
set(CAMOS_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${CAMOS_GEN_DIR}")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(READ "${CAMOS_DEFAULTS_JSON}" _json)
    foreach(_k apiBase iotHost kvsRegion timezone apn ssid password)
        string(JSON _v ERROR_VARIABLE _err GET "${_json}" ${_k})
        if(_err)
            set(_v "")
        endif()
        string(REPLACE "\\" "\\\\" _v "${_v}")
        string(REPLACE "\"" "\\\"" _v "${_v}")
        set(D_${_k} "${_v}")
    endforeach()
    foreach(_k intervalMin)
        string(JSON _v ERROR_VARIABLE _err GET "${_json}" ${_k})
        if(_err OR NOT _v MATCHES "^[0-9]+$")
            set(_v 60)
        endif()
        set(D_${_k} "${_v}")
    endforeach()
    foreach(_k cellular gps ledEnabled batteryMonitor)
        string(JSON _v ERROR_VARIABLE _err GET "${_json}" ${_k})
        if(_err OR NOT _v)
            set(D_${_k} 0)
        else()
            set(D_${_k} 1)
        endif()
    endforeach()
    string(JSON _pm ERROR_VARIABLE _err GET "${_json}" powerMode)
    if(_pm STREQUAL "battery")
        set(D_powerModeNum 1)
    elseif(_pm STREQUAL "powered")
        set(D_powerModeNum 2)
    else()
        set(D_powerModeNum 0)
    endif()
    configure_file("${CMAKE_CURRENT_LIST_DIR}/camos_defaults.h.in" "${CAMOS_GEN_DIR}/camos_defaults.h" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CAMOS_DEFAULTS_JSON}" "${CMAKE_CURRENT_LIST_DIR}/camos_defaults.h.in")
    message(STATUS "camos defaults: ${CAMOS_DEFAULTS_NAME} (api ${D_apiBase}, wifi '${D_ssid}')")
endif()

idf_component_register(
    SRCS main.c battery.c power.c power_decide.c store.c wifi.c modem.c net.c portal.c web.c led.c qr.c register.c apn.c
         reg_parse.c cam.c flybot.c ota.c proto.c shadow.c sched.c util.c tz.c nmea.c gps.c live.c console.c
         stream.c sd.c motion.c motion_core.c events.c hostname.c
    INCLUDE_DIRS "." "${CAMOS_GEN_DIR}"
    EMBED_TXTFILES certs/AmazonRootCA1.pem portal.html console.html
)

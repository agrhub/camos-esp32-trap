# SPDX-License-Identifier: AGPL-3.0-only
# Host unit tests (no ESP-IDF build needed): make -C tests (NMEA, power, registration, motion, SigV4, live data channel,
# remote update / pause policy, remote log, boot photo, battery under a charger)
# test_reg and test_live_dc need cJSON: ESP-IDF's copy when IDF_PATH (or C:/dev/esp-idf) has it, else the system library (libcjson-dev).
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O1
M = ../firmware/main
IDF_PATH ?= C:/dev/esp-idf
CJSON_DIR ?= $(IDF_PATH)/components/json/cJSON
ifneq ($(wildcard $(CJSON_DIR)/cJSON.c),)
CJSON_CFLAGS = -I$(CJSON_DIR)
CJSON_SRC = $(CJSON_DIR)/cJSON.c
CJSON_LIBS =
else
CJSON_CFLAGS = -I/usr/include/cjson
CJSON_SRC =
CJSON_LIBS = -lcjson
endif

all: test_nmea test_power test_reg test_motion test_sigv4 test_live_dc test_fleet test_logfwd test_sched test_battery
	./test_nmea
	./test_power
	./test_reg
	./test_motion
	./test_sigv4
	./test_live_dc
	./test_fleet
	./test_logfwd
	./test_sched
	./test_battery

test_nmea: test_nmea.c $(M)/nmea.c $(M)/nmea.h
	$(CC) $(CFLAGS) -o $@ test_nmea.c $(M)/nmea.c -lm

test_power: test_power.c $(M)/power_decide.c $(M)/power_decide.h
	$(CC) $(CFLAGS) -o $@ test_power.c $(M)/power_decide.c

test_reg: test_reg.c $(M)/reg_parse.c $(M)/reg_parse.h $(M)/apn.c $(M)/apn.h $(M)/nmea.c
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_reg.c $(M)/reg_parse.c $(M)/apn.c $(M)/nmea.c $(CJSON_SRC) $(CJSON_LIBS) -lm

test_motion: test_motion.c $(M)/motion_core.c $(M)/motion_core.h
	$(CC) $(CFLAGS) -o $@ test_motion.c $(M)/motion_core.c

test_sigv4: test_sigv4.c $(M)/sigv4.c $(M)/sigv4.h
	$(CC) $(CFLAGS) -o $@ test_sigv4.c $(M)/sigv4.c

test_live_dc: test_live_dc.c $(M)/live_dc.c $(M)/live_dc.h
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_live_dc.c $(M)/live_dc.c $(CJSON_SRC) $(CJSON_LIBS) -lm

test_fleet: test_fleet.c $(M)/fleet_policy.c $(M)/fleet_policy.h
	$(CC) $(CFLAGS) -o $@ test_fleet.c $(M)/fleet_policy.c

test_logfwd: test_logfwd.c $(M)/logfwd_core.c $(M)/logfwd_core.h
	$(CC) $(CFLAGS) -o $@ test_logfwd.c $(M)/logfwd_core.c

test_sched: test_sched.c $(M)/sched_policy.h
	$(CC) $(CFLAGS) -o $@ test_sched.c

test_battery: test_battery.c $(M)/battery_est.h
	$(CC) $(CFLAGS) -o $@ test_battery.c

clean:
	rm -f test_nmea test_power test_reg test_motion test_sigv4 test_live_dc test_fleet test_logfwd test_sched test_battery *.exe

.PHONY: all clean

# Host unit tests (no ESP-IDF build needed): make -C tests
# test_reg needs cJSON: ESP-IDF's copy when IDF_PATH (or C:/dev/esp-idf) has it, else the system library (libcjson-dev).
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

all: test_nmea test_power test_reg test_motion
	./test_nmea
	./test_power
	./test_reg
	./test_motion

test_nmea: test_nmea.c $(M)/nmea.c $(M)/nmea.h
	$(CC) $(CFLAGS) -o $@ test_nmea.c $(M)/nmea.c -lm

test_power: test_power.c $(M)/power_decide.c $(M)/power_decide.h
	$(CC) $(CFLAGS) -o $@ test_power.c $(M)/power_decide.c

test_reg: test_reg.c $(M)/reg_parse.c $(M)/reg_parse.h $(M)/apn.c $(M)/apn.h $(M)/nmea.c
	$(CC) $(CFLAGS) $(CJSON_CFLAGS) -o $@ test_reg.c $(M)/reg_parse.c $(M)/apn.c $(M)/nmea.c $(CJSON_SRC) $(CJSON_LIBS) -lm

test_motion: test_motion.c $(M)/motion_core.c $(M)/motion_core.h
	$(CC) $(CFLAGS) -o $@ test_motion.c $(M)/motion_core.c

clean:
	rm -f test_nmea test_power test_reg test_motion *.exe

.PHONY: all clean

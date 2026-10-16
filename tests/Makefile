# Host unit tests (no ESP-IDF needed): make -C tests
CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -O1
M = ../firmware/main

all: test_nmea test_power
	./test_nmea
	./test_power

test_nmea: test_nmea.c $(M)/nmea.c $(M)/nmea.h
	$(CC) $(CFLAGS) -o $@ test_nmea.c $(M)/nmea.c -lm

test_power: test_power.c $(M)/power_decide.c $(M)/power_decide.h
	$(CC) $(CFLAGS) -o $@ test_power.c $(M)/power_decide.c

clean:
	rm -f test_nmea test_power test_nmea.exe test_power.exe

.PHONY: all clean

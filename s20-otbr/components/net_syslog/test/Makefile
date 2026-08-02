# Host tests for the net_syslog component.
#
# These run on the build machine, not on the ESP32: the FreeRTOS queue/task API
# is stubbed with pthreads (rtos_stub.c) and sendto() is redirected to a fake,
# so the component's logic can be exercised in seconds without hardware.
#
#   make            build and run
#   make sanitize   the same, under AddressSanitizer + UBSan
#   make clean

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O1 -g
WARN     = -Wall -Wextra -Wpedantic -Wshadow
# The component is #included by the test, so its statics are visible; anything
# it does not use would otherwise trip -Wunused-function.
WARN    += -Wno-unused-function
COMPONENT = ..
INCLUDES = -I stubs -I $(COMPONENT)/src -I $(COMPONENT)/include
# Redirect the socket call to the fake, so a test can assert which thread it
# ran on. This is the check that guards against re-introducing the lwip
# deadlock that a clean compile cannot catch.
DEFINES  = -Dsendto=fake_sendto
LDFLAGS  = -pthread

SOURCES  = test_net_syslog.c rtos_stub.c
BINARY   = test_net_syslog

.PHONY: all sanitize clean

all: $(BINARY)
	./$(BINARY)

$(BINARY): $(SOURCES) $(COMPONENT)/src/net_syslog.c $(COMPONENT)/include/net_syslog.h
	$(CC) $(CFLAGS) $(WARN) $(INCLUDES) $(DEFINES) -o $@ $(SOURCES) $(LDFLAGS)

sanitize: $(SOURCES) $(COMPONENT)/src/net_syslog.c
	$(CC) $(CFLAGS) $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer \
		$(INCLUDES) $(DEFINES) -o $(BINARY)_sanitize $(SOURCES) $(LDFLAGS)
	./$(BINARY)_sanitize

clean:
	rm -rf $(BINARY) $(BINARY)_sanitize $(BINARY).dSYM $(BINARY)_sanitize.dSYM

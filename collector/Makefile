# Makefile for gatotray collector daemon
# Zero-dependency C daemon

CC := gcc
CFLAGS := -std=c99 -Wall -Wextra -O2 -g -D_GNU_SOURCE
LDFLAGS := 

TARGETS := collector client
COLLECTOR_SOURCES := collector.c
CLIENT_SOURCES := client.c
COLLECTOR_OBJECTS := $(COLLECTOR_SOURCES:.c=.o)
CLIENT_OBJECTS := $(CLIENT_SOURCES:.c=.o)

.PHONY: all clean install

all: $(TARGETS)

collector: $(COLLECTOR_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

client: $(CLIENT_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGETS) $(COLLECTOR_OBJECTS) $(CLIENT_OBJECTS)

install: $(TARGETS)
	install -D -m 755 collector $(DESTDIR)/usr/bin/gatotray-collector
	install -D -m 755 client $(DESTDIR)/usr/bin/gatotray-client

# Test target to verify the collector builds
test: $(TARGETS)
	@echo "Build successful - collector and client are ready"
	@./collector -h
	@echo ""
	@./client -h || true

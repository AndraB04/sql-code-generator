CC=gcc
CFLAGS=-g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -Iinclude
LIBCONFIG_CFLAGS=$(shell pkg-config --cflags libconfig 2>/dev/null)
LIBCONFIG_LIBS=$(shell pkg-config --libs libconfig 2>/dev/null)
ifneq ($(strip $(LIBCONFIG_LIBS)),)
CFLAGS+=-DHAVE_LIBCONFIG $(LIBCONFIG_CFLAGS)
endif
LDFLAGS=-pthread $(LIBCONFIG_LIBS)
NCURSES_LIBS=-lncurses -ltinfo

COMMON=src/protocol.c src/model.c

.PHONY: all clean run-server

all: server client admin

server: src/server.c $(COMMON) include/protocol.h include/model.h
	$(CC) $(CFLAGS) src/server.c $(COMMON) $(LDFLAGS) -o server

client: src/client.c src/protocol.c include/protocol.h
	$(CC) $(CFLAGS) src/client.c src/protocol.c -o client

admin: src/admin_client.c src/protocol.c include/protocol.h
	$(CC) $(CFLAGS) src/admin_client.c src/protocol.c $(NCURSES_LIBS) -o admin

run-server: server
	./server

clean:
	rm -f server client admin
	rm -f data/upload_*.schema.json

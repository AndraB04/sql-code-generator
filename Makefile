CC=gcc
CFLAGS=-g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -Iinclude
LDFLAGS=-pthread -lconfig
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

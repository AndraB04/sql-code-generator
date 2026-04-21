# Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
# IR3 2026, grupa 2
# Makefile
# In acest fisier definim regulile de compilare pentru server,
# clientul obisnuit si clientul admin al aplicatiei SQL Code Generator.

# compilatorul C folosit pentru toate tintele
CC=gcc

# optiuni comune de compilare:
# -g adauga informatii de debug
# -Wall si -Wextra activeaza avertismente utile
# -std=c11 selecteaza standardul C folosit
# -D_DEFAULT_SOURCE activeaza extensii POSIX necesare pe Linux
# -Iinclude adauga directorul cu headerele proiectului
CFLAGS=-g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -Iinclude

# detectam optional biblioteca libconfig prin pkg-config
LIBCONFIG_CFLAGS=$(shell pkg-config --cflags libconfig 2>/dev/null)
LIBCONFIG_LIBS=$(shell pkg-config --libs libconfig 2>/dev/null)

# daca libconfig exista, activam codul conditionat de HAVE_LIBCONFIG
ifneq ($(strip $(LIBCONFIG_LIBS)),)
CFLAGS+=-DHAVE_LIBCONFIG $(LIBCONFIG_CFLAGS)
endif

# biblioteci folosite la link-editarea serverului
LDFLAGS=-pthread $(LIBCONFIG_LIBS)

# biblioteci folosite de clientul admin pentru interfata ncurses
NCURSES_LIBS=-lncurses -ltinfo

# fisiere sursa comune pentru server si client
COMMON=src/protocol.c src/model.c

# tintele care nu corespund unor fisiere reale
.PHONY: all clean run-server

# tinta implicita construieste toate executabilele
all: server client admin

# construieste serverul TCP principal
server: src/server.c $(COMMON) include/protocol.h include/model.h
	$(CC) $(CFLAGS) src/server.c $(COMMON) $(LDFLAGS) -o server

# construieste clientul obisnuit
client: src/client.c src/protocol.c include/protocol.h
	$(CC) $(CFLAGS) src/client.c src/protocol.c -o client

# construieste clientul admin cu interfata ncurses
admin: src/admin_client.c src/protocol.c include/protocol.h
	$(CC) $(CFLAGS) src/admin_client.c src/protocol.c $(NCURSES_LIBS) -o admin

# compileaza serverul, apoi il porneste local
run-server: server
	./server

# sterge executabilele si fisierele temporare create la upload
clean:
	rm -f server client admin
	rm -f data/upload_*.schema.json

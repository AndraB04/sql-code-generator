# Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
# IR3 2026, grupa 2
# Makefile
# In acest fisier definim regulile de compilare pentru server,
# clientul obisnuit si clientul admin al aplicatiei SQL Code Generator.

# compilatorul C folosit pentru toate tintele
CC=gcc

# optiuni comune de compilare:
# -g adauga informatii de debug
# -Wall, -Wextra, -Wpedantic si -Werror activeaza verificari stricte
# -std=c11 selecteaza standardul C folosit
# -D_POSIX_C_SOURCE=200809L expune API-urile POSIX cerute
# -Iinclude adauga directorul cu headerele proiectului
CFLAGS=-g -Wall -Wextra -Wpedantic -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude

# detectam optional biblioteca libconfig prin pkg-config
LIBCONFIG_CFLAGS=$(shell pkg-config --cflags libconfig 2>/dev/null)
LIBCONFIG_LIBS=$(shell pkg-config --libs libconfig 2>/dev/null)

# bibliotecile cerute de tema; pot fi instalate global sau extrase local in third_party/local
THIRD_PARTY_INC=third_party/local/usr/include
THIRD_PARTY_LIB=third_party/local/usr/lib/x86_64-linux-gnu
EXT_CFLAGS=-I$(THIRD_PARTY_INC)
EXT_LIBS=-L$(THIRD_PARTY_LIB) -Wl,-rpath,$(CURDIR)/$(THIRD_PARTY_LIB) -lcjson -lpg_query

CFLAGS+=$(EXT_CFLAGS)

# daca libconfig exista, activam codul conditionat de HAVE_LIBCONFIG
ifneq ($(strip $(LIBCONFIG_LIBS)),)
CFLAGS+=-DHAVE_LIBCONFIG $(LIBCONFIG_CFLAGS)
endif

# biblioteci folosite la link-editarea serverului
LDFLAGS=-pthread $(LIBCONFIG_LIBS) $(EXT_LIBS)

# biblioteci folosite de clientul admin pentru interfata ncurses
NCURSES_LIBS=-lncurses -ltinfo

# fisiere sursa comune pentru server si client
COMMON=src/protocol.c src/model.c

# tintele care nu corespund unor fisiere reale
.PHONY: all clean run-server docs clean-docs clang-tidy asan tsan memcheck helgrind

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

# ruleaza analiza statica stricta daca clang-tidy este instalat
clang-tidy:
	clang-tidy src/server.c src/client.c src/admin_client.c src/protocol.c src/model.c -- $(CFLAGS)

# build cu Address/Leak/Undefined Sanitizer
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-g -O0 -fno-omit-frame-pointer -fsanitize=address,leak,undefined -Wall -Wextra -Wpedantic -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude $(EXT_CFLAGS)" \
	        LDFLAGS="-pthread -fsanitize=address,leak,undefined $(LIBCONFIG_LIBS) $(EXT_LIBS)" all

# build cu Thread Sanitizer
tsan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-g -O0 -fno-omit-frame-pointer -fsanitize=thread -Wall -Wextra -Wpedantic -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude $(EXT_CFLAGS)" \
	        LDFLAGS="-pthread -fsanitize=thread $(LIBCONFIG_LIBS) $(EXT_LIBS)" all

# ruleaza Valgrind Memcheck pe server
memcheck: server
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --child-silent-after-fork=yes ./server

# ruleaza Valgrind Helgrind pe server
helgrind: server
	valgrind --tool=helgrind --child-silent-after-fork=yes ./server

# genereaza PDF-urile documentatiei pentru Milestone 2 din sursele Markdown
docs: docs/SRS.pdf docs/SDD.pdf
	@echo "Documentation PDFs generated."

docs/%.pdf: docs/%.md tools/md_to_pdf.py
	python3 tools/md_to_pdf.py $< $@

# sterge fisierele auxiliare generate de pdflatex
clean-docs:
	rm -f docs/*.aux docs/*.log docs/*.out docs/*.toc docs/*.tex

# sterge executabilele si fisierele temporare create la upload
clean:
	rm -f server client admin
	rm -f data/upload_*.schema.json
	rm -f data/generated_*.sql
	rm -f logs/server.log
	rm -f docs/*.aux docs/*.log docs/*.out docs/*.toc docs/*.tex

# rularea se face cu make all

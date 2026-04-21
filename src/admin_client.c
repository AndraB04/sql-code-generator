/**
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * Client TCP Admin - interfata ncurses
 * In acest program implementam un client admin care se conecteaza la server
 * prin TCP si afiseaza rapoarte folosind o interfata text (ncurses)
 * Utilizatorul poate selecta diferite statistici din meniu
 */

#include "protocol.h"  /* defineste functii si structuri pentru comunicatia client-server */

#include <ncurses.h>   /* biblioteca pentru interfata text interactiva */
#include <stdio.h>     /* functii standard de intrare/iesire */
#include <stdlib.h>    /* gestionare memorie si conversii */
#include <string.h>    /* manipulare siruri de caractere */
#include <unistd.h>    /* apeluri POSIX: close */

/* functie care parseaza un port din string in uint16_t */
static int parse_port(const char *value, uint16_t *port) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10); /* convertim string -> long */
    
    /* verificam daca conversia este valida si portul este in intervalul permis */
    if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }

    *port = (uint16_t)parsed; /* salvam portul convertit */
    return 0;
}

/* functie care afiseaza modul de utilizare al programului */
static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [host] [--host host] [--port admin_port]\n", prog);
}

/* functie care trimite o cerere text catre server si asteapta raspuns */
static int request_text(int fd, uint32_t op, const char *payload, char **reply) {
    
    /* trimitem mesajul catre server */
    if (send_message(fd, 0, op, payload, payload ? (uint32_t)strlen(payload) : 0) < 0) {
        return -1;
    }

    MsgHeader h;   /* header-ul mesajului primit */
    char *resp = NULL;

    /* receptionam raspunsul de la server */
    int rc = recv_message(fd, &h, &resp);
    if (rc <= 0) {
        free(resp);
        return -1;
    }

    *reply = resp; /* salvam raspunsul */

    /* verificam daca operatia a fost cu succes */
    return h.op_id == OP_OK ? 0 : -1;
}

/* functie care deseneaza meniul in interfata ncurses */
static void draw_menu(const char *report) {
    erase(); /* curatam ecranul */

    /* afisam titlul si optiunile */
    mvprintw(0, 0, "SQL Code Generator Admin");
    mvprintw(2, 0, "1 clients   2 commands   3 avg time   4 history   5 tables   6 queue   q quit");

    /* linie de separare */
    mvhline(3, 0, '-', COLS);

    /* afisam continutul raportului */
    mvprintw(4, 0, "%s", report != NULL ? report : "Select a report.");

    refresh(); /* actualizam ecranul */
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";     /* host implicit */
    uint16_t port = SQLCG_ADMIN_PORT;   /* port implicit */

    /* parcurgem argumentele din linia de comanda */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        /* setam host-ul */
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];

        /* setam portul */
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_port(argv[++i], &port) < 0) {
                fprintf(stderr, "invalid admin port\n");
                return 1;
            }

        /* daca se da direct host fara flag */
        } else if (argv[i][0] != '-') {
            host = argv[i];

        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    /* realizam conexiunea TCP catre server */
    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to admin port %s:%u\n", host, (unsigned)port);
        return 1;
    }

    char *reply = NULL;

    /* trimitem cerere de autentificare admin */
    if (request_text(fd, OP_ADMIN_LOGIN, "admin", &reply) < 0) {
        fprintf(stderr, "%s\n", reply ? reply : "admin login failed");
        free(reply);
        close(fd);
        return 1;
    }
    free(reply);

    /* initializam interfata ncurses */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    char report[65536];

    /* mesaj initial */
    snprintf(report, sizeof(report), "Connected to %s:%u", host, (unsigned)port);
    draw_menu(report);

    /* bucla principala pentru input utilizator */
    for (;;) {
        int ch = getch(); /* citim tasta apasata */
        const char *category = NULL;

        /* mapam tastele la tipuri de rapoarte */
        if (ch == '1') {
            category = "clients";
        } else if (ch == '2') {
            category = "commands";
        } else if (ch == '3') {
            category = "avg";
        } else if (ch == '4') {
            category = "history";
        } else if (ch == '5') {
            category = "tables";
        } else if (ch == '6') {
            category = "queue";
        } else if (ch == 'q' || ch == 'Q') {
            break; /* iesire din program */
        } else {
            continue;
        }

        reply = NULL;

        /* cerem raportul de la server */
        if (request_text(fd, OP_ADMIN_REPORT, category, &reply) == 0) {
            snprintf(report, sizeof(report), "%s", reply ? reply : "");
        } else {
            snprintf(report, sizeof(report), "%s", reply ? reply : "request failed");
        }

        free(reply);

        /* redesenam meniul cu noul continut */
        draw_menu(report);
    }

    /* inchidem interfata ncurses */
    endwin();

    /* trimitem mesaj de inchidere catre server */
    request_text(fd, OP_ADMIN_BYE, "", &reply);
    free(reply);

    /* inchidem socket-ul */
    close(fd);

    return 0;
}

#include "protocol.h"

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_port(const char *value, uint16_t *port) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }
    *port = (uint16_t)parsed;
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [host] [--host host] [--port admin_port]\n", prog);
}

static int request_text(int fd, uint32_t op, const char *payload, char **reply) {
    if (send_message(fd, 0, op, payload, payload ? (uint32_t)strlen(payload) : 0) < 0) {
        return -1;
    }
    MsgHeader h;
    char *resp = NULL;
    int rc = recv_message(fd, &h, &resp);
    if (rc <= 0) {
        free(resp);
        return -1;
    }
    *reply = resp;
    return h.op_id == OP_OK ? 0 : -1;
}

static void draw_menu(const char *report) {
    erase();
    mvprintw(0, 0, "SQL Code Generator Admin");
    mvprintw(2, 0, "1 clients   2 commands   3 avg time   4 history   5 tables   6 queue   q quit");
    mvhline(3, 0, '-', COLS);
    mvprintw(4, 0, "%s", report != NULL ? report : "Select a report.");
    refresh();
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    uint16_t port = SQLCG_ADMIN_PORT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_port(argv[++i], &port) < 0) {
                fprintf(stderr, "invalid admin port\n");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            host = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to admin port %s:%u\n", host, (unsigned)port);
        return 1;
    }

    char *reply = NULL;
    if (request_text(fd, OP_ADMIN_LOGIN, "admin", &reply) < 0) {
        fprintf(stderr, "%s\n", reply ? reply : "admin login failed");
        free(reply);
        close(fd);
        return 1;
    }
    free(reply);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    char report[65536];
    snprintf(report, sizeof(report), "Connected to %s:%u", host, (unsigned)port);
    draw_menu(report);

    for (;;) {
        int ch = getch();
        const char *category = NULL;
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
            break;
        } else {
            continue;
        }

        reply = NULL;
        if (request_text(fd, OP_ADMIN_REPORT, category, &reply) == 0) {
            snprintf(report, sizeof(report), "%s", reply ? reply : "");
        } else {
            snprintf(report, sizeof(report), "%s", reply ? reply : "request failed");
        }
        free(reply);
        draw_menu(report);
    }

    endwin();
    request_text(fd, OP_ADMIN_BYE, "", &reply);
    free(reply);
    close(fd);
    return 0;
}

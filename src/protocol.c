/*
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * protocol.c
 * In acest fisier implementam functiile comune de comunicatie TCP:
 * conectare, creare socket de ascultare, citire/scriere completa
 * si transmiterea mesajelor formate din header + payload.
 */

#include "protocol.h"   /* constantele protocolului si structura MsgHeader */

#include <arpa/inet.h>  /* conversii host/network byte order si sockaddr_in */
#include <errno.h>      /* coduri de eroare, inclusiv EINTR */
#include <netdb.h>      /* getaddrinfo si struct addrinfo */
#include <stdio.h>      /* perror, fprintf */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* memset */
#include <sys/socket.h> /* socket, bind, listen, connect, recv */
#include <sys/types.h>  /* tipuri POSIX folosite de socketuri */
#include <unistd.h>     /* write, close */

/*
 * citeste exact len octeti din descriptorul fd
 * returneaza:
 *  1 daca s-au citit toti octetii ceruti
 *  0 daca peer-ul a inchis conexiunea
 * -1 la eroare
 */
int read_full(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;

    /* recv poate intoarce mai putini octeti decat am cerut, deci repetam */
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);

        /* n == 0 inseamna inchiderea ordonata a conexiunii */
        if (n == 0) {
            return 0;
        }

        /* semnalele intrerup recv, dar operatia poate fi reluata */
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

/*
 * scrie exact len octeti catre descriptorul fd
 * returneaza 0 la succes si -1 la eroare
 */
int write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;

    /* write poate trimite partial bufferul, deci avansam pana la final */
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);

        /* EINTR nu este eroare fatala: reluam scrierea */
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/*
 * creeaza un socket TCP server care asculta pe toate interfetele
 * portul este primit in format host byte order si convertit cu htons
 */
int listen_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;

    /* permite repornirea rapida a serverului pe acelasi port */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* atasam socketul la portul cerut */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* backlog-ul 128 limiteaza conexiunile acceptate in asteptare */
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * deschide o conexiune TCP catre host:port
 * getaddrinfo transforma host-ul si portul in adrese utilizabile de connect
 */
int connect_tcp(const char *host, uint16_t port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char service[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    /*
     * incercam fiecare adresa returnata pana cand una accepta conexiunea
     * daca o incercare esueaza, inchidem socketul si trecem la urmatoarea
     */
    int fd = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/*
 * trimite un mesaj complet in formatul protocolului:
 * mai intai header-ul in network byte order, apoi payload-ul optional
 */
int send_message(int fd, uint32_t client_id, uint32_t op_id, const void *payload, uint32_t size) {
    MsgHeader h;

    /* toate campurile numerice sunt convertite pentru transmitere prin retea */
    h.msg_size = htonl(size);
    h.client_id = htonl(client_id);
    h.op_id = htonl(op_id);
    h.flags = 0;

    if (write_full(fd, &h, sizeof(h)) < 0) {
        return -1;
    }
    if (size > 0 && payload != NULL) {
        return write_full(fd, payload, size);
    }
    return 0;
}

/*
 * receptioneaza un mesaj complet si aloca dinamic payload-ul
 * apelantul trebuie sa elibereze memoria prin free()
 */
int recv_message(int fd, MsgHeader *header, char **payload) {
    MsgHeader net;

    /* citim mai intai header-ul fix */
    int rc = read_full(fd, &net, sizeof(net));
    if (rc <= 0) {
        return rc;
    }

    /* convertim campurile din network byte order in formatul local */
    header->msg_size = ntohl(net.msg_size);
    header->client_id = ntohl(net.client_id);
    header->op_id = ntohl(net.op_id);
    header->flags = ntohl(net.flags);

    /* protejam serverul/clientul de payload-uri mai mari decat limita protocolului */
    if (header->msg_size > SQLCG_MAX_PAYLOAD) {
        return -1;
    }

    *payload = NULL;
    if (header->msg_size > 0) {
        /* alocam un octet in plus ca sa putem termina payload-ul ca sir C */
        *payload = (char *)malloc((size_t)header->msg_size + 1);
        if (*payload == NULL) {
            return -1;
        }

        /* daca citirea payload-ului esueaza, curatam memoria alocata */
        rc = read_full(fd, *payload, header->msg_size);
        if (rc <= 0) {
            free(*payload);
            *payload = NULL;
            return rc;
        }
        (*payload)[header->msg_size] = '\0';
    }
    return 1;
}

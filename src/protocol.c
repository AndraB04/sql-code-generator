#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int read_full(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0) {
            return 0;
        }
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

int write_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
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

int listen_tcp(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
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
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

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

int send_message(int fd, uint32_t client_id, uint32_t op_id, const void *payload, uint32_t size) {
    MsgHeader h;
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

int recv_message(int fd, MsgHeader *header, char **payload) {
    MsgHeader net;
    int rc = read_full(fd, &net, sizeof(net));
    if (rc <= 0) {
        return rc;
    }

    header->msg_size = ntohl(net.msg_size);
    header->client_id = ntohl(net.client_id);
    header->op_id = ntohl(net.op_id);
    header->flags = ntohl(net.flags);

    if (header->msg_size > SQLCG_MAX_PAYLOAD) {
        return -1;
    }

    *payload = NULL;
    if (header->msg_size > 0) {
        *payload = (char *)malloc((size_t)header->msg_size + 1);
        if (*payload == NULL) {
            return -1;
        }
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

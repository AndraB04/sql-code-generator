#ifndef SQLCG_PROTOCOL_H
#define SQLCG_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define SQLCG_PORT 18081
#define SQLCG_ADMIN_PORT 18082
#define SQLCG_MAX_PAYLOAD (1024u * 1024u)
#define SQLCG_FILE_CHUNK (64u * 1024u)

enum {
    OP_CONNECT = 0,
    OP_BYE = 5,
    OP_UPLOAD_BEGIN = 10,
    OP_UPLOAD_CHUNK = 11,
    OP_UPLOAD_END = 12,
    OP_GENERATE_SQL = 20,
    OP_VALIDATE_INSERT = 30,
    OP_OK = 40,
    OP_ERROR = 41,

    OP_ADMIN_LOGIN = 100,
    OP_ADMIN_REPORT = 101,
    OP_ADMIN_BYE = 105
};

typedef struct {
    uint32_t msg_size;
    uint32_t client_id;
    uint32_t op_id;
    uint32_t flags;
} MsgHeader;

int connect_tcp(const char *host, uint16_t port);
int listen_tcp(uint16_t port);
int read_full(int fd, void *buf, size_t len);
int write_full(int fd, const void *buf, size_t len);
int send_message(int fd, uint32_t client_id, uint32_t op_id, const void *payload, uint32_t size);
int recv_message(int fd, MsgHeader *header, char **payload);

#endif

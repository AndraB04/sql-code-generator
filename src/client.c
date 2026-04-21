#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_port(const char *value, uint16_t *port) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }
    *port = (uint16_t)parsed;
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [host] [--host host] [--port port] [--input schema.json]\n"
            "          [--generate] [--insert-file batch.sql] [--no-repl]\n",
            prog);
}

static int request_text(int fd, uint32_t client_id, uint32_t op, const char *payload, char **reply) {
    if (send_message(fd, client_id, op, payload, payload ? (uint32_t)strlen(payload) : 0) < 0) {
        return -1;
    }
    MsgHeader h;
    char *resp = NULL;
    int rc = recv_message(fd, &h, &resp);
    if (rc <= 0) {
        free(resp);
        return -1;
    }
    if (reply != NULL) {
        *reply = resp;
    } else {
        printf("%s\n", resp ? resp : "");
        free(resp);
    }
    return h.op_id == OP_OK ? 0 : -1;
}

static int connect_protocol(int fd, uint32_t *client_id) {
    char *reply = NULL;
    if (request_text(fd, 0, OP_CONNECT, "", &reply) < 0) {
        free(reply);
        return -1;
    }
    *client_id = (uint32_t)strtoul(reply, NULL, 10);
    free(reply);
    return *client_id == 0 ? -1 : 0;
}

static int upload_file(int fd, uint32_t client_id, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (request_text(fd, client_id, OP_UPLOAD_BEGIN, path, NULL) < 0) {
        fclose(f);
        return -1;
    }

    char *buf = (char *)malloc(SQLCG_FILE_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }

    for (;;) {
        size_t n = fread(buf, 1, SQLCG_FILE_CHUNK, f);
        if (n > 0) {
            if (send_message(fd, client_id, OP_UPLOAD_CHUNK, buf, (uint32_t)n) < 0) {
                free(buf);
                fclose(f);
                return -1;
            }
            MsgHeader h;
            char *resp = NULL;
            if (recv_message(fd, &h, &resp) <= 0 || h.op_id != OP_OK) {
                fprintf(stderr, "%s\n", resp ? resp : "upload chunk failed");
                free(resp);
                free(buf);
                fclose(f);
                return -1;
            }
            free(resp);
        }
        if (n < SQLCG_FILE_CHUNK) {
            if (ferror(f)) {
                fprintf(stderr, "read failed: %s\n", strerror(errno));
                free(buf);
                fclose(f);
                return -1;
            }
            break;
        }
    }

    free(buf);
    fclose(f);
    return request_text(fd, client_id, OP_UPLOAD_END, "", NULL);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len < 0 || len > (long)SQLCG_MAX_PAYLOAD) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static void print_help(void) {
    puts("Commands:");
    puts("  upload <schema.json>");
    puts("  generate");
    puts("  insert <INSERT INTO ...>");
    puts("  insertfile <batch.sql>");
    puts("  quit");
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *input_path = NULL;
    const char *insert_path = NULL;
    uint16_t port = SQLCG_PORT;
    int generate_once = 0;
    int no_repl = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_port(argv[++i], &port) < 0) {
                fprintf(stderr, "invalid port\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--insert-file") == 0 && i + 1 < argc) {
            insert_path = argv[++i];
        } else if (strcmp(argv[i], "--generate") == 0) {
            generate_once = 1;
        } else if (strcmp(argv[i], "--no-repl") == 0) {
            no_repl = 1;
        } else if (argv[i][0] != '-') {
            host = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:%u\n", host, (unsigned)port);
        return 1;
    }

    uint32_t client_id = 0;
    if (connect_protocol(fd, &client_id) < 0) {
        fprintf(stderr, "protocol connect failed\n");
        close(fd);
        return 1;
    }
    printf("Connected as client %u\n", client_id);

    if (input_path != NULL && upload_file(fd, client_id, input_path) < 0) {
        close(fd);
        return 1;
    }
    if (generate_once && request_text(fd, client_id, OP_GENERATE_SQL, "", NULL) < 0) {
        close(fd);
        return 1;
    }
    if (insert_path != NULL) {
        char *sql = read_file(insert_path);
        if (sql == NULL) {
            fprintf(stderr, "cannot read insert file\n");
            close(fd);
            return 1;
        }
        int rc = request_text(fd, client_id, OP_VALIDATE_INSERT, sql, NULL);
        free(sql);
        if (rc < 0) {
            close(fd);
            return 1;
        }
    }
    if (no_repl) {
        request_text(fd, client_id, OP_BYE, "", NULL);
        close(fd);
        return 0;
    }

    print_help();

    char line[8192];
    for (;;) {
        printf("sqlcg> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "upload ", 7) == 0) {
            upload_file(fd, client_id, line + 7);
        } else if (strcmp(line, "generate") == 0) {
            request_text(fd, client_id, OP_GENERATE_SQL, "", NULL);
        } else if (strncmp(line, "insert ", 7) == 0) {
            request_text(fd, client_id, OP_VALIDATE_INSERT, line + 7, NULL);
        } else if (strncmp(line, "insertfile ", 11) == 0) {
            char *sql = read_file(line + 11);
            if (sql == NULL) {
                fprintf(stderr, "cannot read insert file\n");
            } else {
                request_text(fd, client_id, OP_VALIDATE_INSERT, sql, NULL);
                free(sql);
            }
        } else if (strcmp(line, "help") == 0) {
            print_help();
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            request_text(fd, client_id, OP_BYE, "", NULL);
            break;
        } else if (line[0] != '\0') {
            print_help();
        }
    }

    close(fd);
    return 0;
}

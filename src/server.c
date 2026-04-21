#include "model.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBCONFIG
#include <libconfig.h>
#endif

#define MAX_FDS 256
#define DEFAULT_ADMIN_TIMEOUT_SEC 60

typedef struct {
    uint16_t server_port;
    uint16_t admin_port;
    int admin_timeout_sec;
    char config_path[256];
} RuntimeConfig;

typedef struct Request {
    int fd;
    uint32_t client_id;
    uint32_t op_id;
    char *payload;
    uint32_t payload_size;
    struct Request *next;
} Request;

typedef struct {
    Request *head;
    Request *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} RequestQueue;

static SharedState *g_state;
static RequestQueue g_queue;
static RuntimeConfig g_cfg;

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-c config.cfg] [-p server_port] [-a admin_port] [-t admin_timeout]\n",
            prog);
}

static int parse_number(const char *value, long min, long max, long *out) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }
    *out = parsed;
    return 0;
}

static void apply_port_value(const char *label, const char *value, uint16_t *target) {
    long parsed = 0;
    if (parse_number(value, 1, 65535, &parsed) == 0) {
        *target = (uint16_t)parsed;
    } else {
        fprintf(stderr, "Ignoring invalid %s value '%s'\n", label, value);
    }
}

static void apply_timeout_value(const char *label, const char *value, int *target) {
    long parsed = 0;
    if (parse_number(value, 1, 86400, &parsed) == 0) {
        *target = (int)parsed;
    } else {
        fprintf(stderr, "Ignoring invalid %s value '%s'\n", label, value);
    }
}

static void cfg_defaults(RuntimeConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_port = SQLCG_PORT;
    cfg->admin_port = SQLCG_ADMIN_PORT;
    cfg->admin_timeout_sec = DEFAULT_ADMIN_TIMEOUT_SEC;
    snprintf(cfg->config_path, sizeof(cfg->config_path), "config.cfg");
}

static void cfg_read_config_path(RuntimeConfig *cfg, int argc, char **argv) {
    const char *env_path = getenv("CONFIG_PATH");
    if (env_path != NULL && env_path[0] != '\0') {
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", env_path);
    }
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", argv[++i]);
        }
    }
}

static void cfg_load_file(RuntimeConfig *cfg) {
#ifdef HAVE_LIBCONFIG
    config_t file_cfg;
    config_init(&file_cfg);
    if (!config_read_file(&file_cfg, cfg->config_path)) {
        fprintf(stderr, "Config fallback: cannot read %s:%d - %s\n",
                cfg->config_path, config_error_line(&file_cfg), config_error_text(&file_cfg));
        config_destroy(&file_cfg);
        return;
    }

    int value = 0;
    if (config_lookup_int(&file_cfg, "server.port", &value) && value > 0 && value <= 65535) {
        cfg->server_port = (uint16_t)value;
    }
    if (config_lookup_int(&file_cfg, "server.admin_port", &value) && value > 0 && value <= 65535) {
        cfg->admin_port = (uint16_t)value;
    }
    if (config_lookup_int(&file_cfg, "server.admin_timeout", &value) && value > 0) {
        cfg->admin_timeout_sec = value;
    }
    config_destroy(&file_cfg);
#else
    FILE *f = fopen(cfg->config_path, "r");
    if (f == NULL) {
        fprintf(stderr, "Config fallback: cannot read %s - %s\n", cfg->config_path, strerror(errno));
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        eq++;
        while (*eq == ' ' || *eq == '\t') {
            eq++;
        }
        char value[64];
        size_t len = strcspn(eq, "; \t\r\n");
        if (len == 0 || len >= sizeof(value)) {
            continue;
        }
        memcpy(value, eq, len);
        value[len] = '\0';

        if (strstr(line, "admin_port") != NULL) {
            apply_port_value("server.admin_port", value, &cfg->admin_port);
        } else if (strstr(line, "admin_timeout") != NULL) {
            apply_timeout_value("server.admin_timeout", value, &cfg->admin_timeout_sec);
        } else if (strstr(line, "port") != NULL) {
            apply_port_value("server.port", value, &cfg->server_port);
        }
    }
    fclose(f);
#endif
}

static void cfg_apply_env(RuntimeConfig *cfg) {
    const char *server_port = getenv("SERVER_PORT");
    const char *admin_port = getenv("ADMIN_PORT");
    const char *admin_timeout = getenv("ADMIN_TIMEOUT");
    if (server_port != NULL) {
        apply_port_value("SERVER_PORT", server_port, &cfg->server_port);
    }
    if (admin_port != NULL) {
        apply_port_value("ADMIN_PORT", admin_port, &cfg->admin_port);
    }
    if (admin_timeout != NULL) {
        apply_timeout_value("ADMIN_TIMEOUT", admin_timeout, &cfg->admin_timeout_sec);
    }
}

static int cfg_apply_args(RuntimeConfig *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            i++;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            const char *option = argv[i++];
            apply_port_value(option, argv[i], &cfg->server_port);
        } else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--admin-port") == 0) && i + 1 < argc) {
            const char *option = argv[i++];
            apply_port_value(option, argv[i], &cfg->admin_port);
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--admin-timeout") == 0) && i + 1 < argc) {
            const char *option = argv[i++];
            apply_timeout_value(option, argv[i], &cfg->admin_timeout_sec);
        } else {
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static int cfg_load(RuntimeConfig *cfg, int argc, char **argv) {
    cfg_defaults(cfg);
    cfg_read_config_path(cfg, argc, argv);
    cfg_load_file(cfg);
    cfg_apply_env(cfg);
    return cfg_apply_args(cfg, argc, argv);
}

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void history_add(const char *line) {
    int idx = g_state->history_count % MAX_HISTORY;
    snprintf(g_state->history[idx], MAX_COMMAND, "%s", line);
    g_state->history_count++;
}

static void queue_init(RequestQueue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_push(RequestQueue *q, Request *r) {
    r->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail == NULL) {
        q->head = q->tail = r;
    } else {
        q->tail->next = r;
        q->tail = r;
    }
    g_state->queue_depth++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

static Request *queue_pop(RequestQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    Request *r = q->head;
    q->head = r->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    if (g_state->queue_depth > 0) {
        g_state->queue_depth--;
    }
    pthread_mutex_unlock(&q->mutex);
    return r;
}

static void client_touch(uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].client_id == (int)client_id && g_state->clients[i].fd > 0) {
            g_state->clients[i].last_seen = time(NULL);
            return;
        }
    }
}

static void client_remove_fd(int fd) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].fd == fd) {
            g_state->clients[i].fd = 0;
            if (g_state->client_count > 0) {
                g_state->client_count--;
            }
            return;
        }
    }
}

static void respond_text(int fd, uint32_t client_id, uint32_t op, const char *text) {
    send_message(fd, client_id, op, text, (uint32_t)strlen(text));
}

static void upload_path(uint32_t client_id, char *out, size_t out_len) {
    snprintf(out, out_len, "data/upload_%u.schema.json", client_id);
}

static int append_upload_chunk(uint32_t client_id, const char *payload, uint32_t size, char *err, size_t err_len) {
    char path[128];
    upload_path(client_id, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        snprintf(err, err_len, "cannot append upload file: %s", strerror(errno));
        return -1;
    }
    if (size > 0 && fwrite(payload, 1, size, f) != size) {
        fclose(f);
        snprintf(err, err_len, "cannot write upload chunk");
        return -1;
    }
    fclose(f);
    snprintf(err, err_len, "OK: chunk stored (%u bytes)", size);
    return 0;
}

static int child_validate_insert(const char *sql, char *out, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(out, out_len, "pipe failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(out, out_len, "fork failed");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        char msg[1024];
        int ok = validate_insert_batch(g_state, sql, msg, sizeof(msg)) == 0;
        char packet[1200];
        snprintf(packet, sizeof(packet), "%d\n%s", ok ? 1 : 0, msg);
        write_full(pipefd[1], packet, strlen(packet));
        close(pipefd[1]);
        _exit(ok ? 0 : 1);
    }

    close(pipefd[1]);
    char packet[1200];
    ssize_t n = read(pipefd[0], packet, sizeof(packet) - 1);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0) {
        snprintf(out, out_len, "child did not return validation result");
        return -1;
    }
    packet[n] = '\0';
    int ok = packet[0] == '1';
    char *msg = strchr(packet, '\n');
    snprintf(out, out_len, "%s", msg != NULL ? msg + 1 : packet);
    return ok ? 0 : -1;
}

static void process_request(Request *r) {
    long start = now_ms();
    char response[65536];
    char err[2048];
    response[0] = '\0';
    err[0] = '\0';
    uint32_t resp_op = OP_OK;
    client_touch(r->client_id);

    if (r->op_id == OP_UPLOAD_BEGIN) {
        char path[128];
        upload_path(r->client_id, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            snprintf(response, sizeof(response), "cannot create upload file: %s", strerror(errno));
            resp_op = OP_ERROR;
        } else {
            fclose(f);
            snprintf(response, sizeof(response), "OK: upload started");
        }
        history_add("upload begin");
    } else if (r->op_id == OP_UPLOAD_CHUNK) {
        if (append_upload_chunk(r->client_id, r->payload, r->payload_size, response, sizeof(response)) < 0) {
            resp_op = OP_ERROR;
        }
    } else if (r->op_id == OP_UPLOAD_END) {
        char path[128];
        upload_path(r->client_id, path, sizeof(path));
        if (load_er_json(g_state, path, err, sizeof(err)) == 0) {
            snprintf(response, sizeof(response), "OK: %s", err);
            history_add("schema loaded");
        } else {
            snprintf(response, sizeof(response), "ERROR: %s", err);
            resp_op = OP_ERROR;
            history_add("schema load failed");
        }
    } else if (r->op_id == OP_GENERATE_SQL) {
        generate_sql(g_state, response, sizeof(response));
        history_add("generate sql");
    } else if (r->op_id == OP_VALIDATE_INSERT) {
        g_state->insert_commands++;
        if (r->payload == NULL) {
            snprintf(response, sizeof(response), "empty insert payload");
            resp_op = OP_ERROR;
        } else if (child_validate_insert(r->payload, err, sizeof(err)) == 0) {
            if (apply_insert_batch(g_state, r->payload, err, sizeof(err)) == 0) {
                snprintf(response, sizeof(response), "%s", err);
                history_add("insert accepted");
            } else {
                snprintf(response, sizeof(response), "ERROR: %s", err);
                resp_op = OP_ERROR;
                history_add("insert apply failed");
            }
        } else {
            snprintf(response, sizeof(response), "ERROR: %s", err);
            resp_op = OP_ERROR;
            history_add("insert rejected");
        }
    } else {
        snprintf(response, sizeof(response), "unknown operation %u", r->op_id);
        resp_op = OP_ERROR;
    }

    long elapsed = now_ms() - start;
    g_state->total_commands++;
    g_state->total_exec_ms += elapsed;
    if (resp_op == OP_ERROR) {
        g_state->failed_commands++;
    }
    respond_text(r->fd, r->client_id, resp_op, response);
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        Request *r = queue_pop(&g_queue);
        process_request(r);
        free(r->payload);
        free(r);
    }
    return NULL;
}

static void remove_pollfd(struct pollfd *fds, int *nfds, int idx) {
    int fd = fds[idx].fd;
    close(fd);
    client_remove_fd(fd);
    if (idx < *nfds - 1) {
        memmove(&fds[idx], &fds[idx + 1], (size_t)(*nfds - idx - 1) * sizeof(struct pollfd));
    }
    (*nfds)--;
}

static void register_client(int fd, const char *addr, uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].fd == 0) {
            g_state->clients[i].fd = fd;
            g_state->clients[i].client_id = (int)client_id;
            snprintf(g_state->clients[i].addr, sizeof(g_state->clients[i].addr), "%s", addr);
            g_state->clients[i].connected_at = time(NULL);
            g_state->clients[i].last_seen = g_state->clients[i].connected_at;
            g_state->client_count++;
            return;
        }
    }
}

static void accept_client(int listen_fd, struct pollfd *fds, int *nfds) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        return;
    }
    if (*nfds >= MAX_FDS) {
        close(fd);
        return;
    }
    char ip[64];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    fds[*nfds].fd = fd;
    fds[*nfds].events = POLLIN;
    fds[*nfds].revents = 0;
    (*nfds)++;
    fprintf(stderr, "ordinary client connected from %s\n", ip);
}

static void accept_admin(int listen_fd, struct pollfd *fds, int *nfds, int *admin_fd, long *admin_last) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        return;
    }
    if (*admin_fd > 0) {
        respond_text(fd, 0, OP_ERROR, "another admin client is already connected");
        close(fd);
        return;
    }
    if (*nfds >= MAX_FDS) {
        close(fd);
        return;
    }
    *admin_fd = fd;
    *admin_last = time(NULL);
    g_state->admin_connected = 1;
    fds[*nfds].fd = fd;
    fds[*nfds].events = POLLIN;
    fds[*nfds].revents = 0;
    (*nfds)++;
    fprintf(stderr, "admin client connected\n");
}

static int is_registered_client(uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].client_id == (int)client_id && g_state->clients[i].fd > 0) {
            return 1;
        }
    }
    return 0;
}

static void handle_admin_message(int fd, int *admin_fd, long *admin_last, MsgHeader *h, char *payload) {
    *admin_last = time(NULL);
    if (h->op_id == OP_ADMIN_BYE) {
        respond_text(fd, 0, OP_OK, "admin disconnected");
        *admin_fd = -1;
        g_state->admin_connected = 0;
        return;
    }
    if (h->op_id == OP_ADMIN_LOGIN) {
        if (payload != NULL && strcmp(payload, "admin") == 0) {
            respond_text(fd, 0, OP_OK, "OK: admin logged in");
        } else {
            respond_text(fd, 0, OP_ERROR, "invalid admin token");
        }
        return;
    }
    if (h->op_id == OP_ADMIN_REPORT) {
        char report[65536];
        admin_report(g_state, payload != NULL ? payload : "", report, sizeof(report));
        respond_text(fd, 0, OP_OK, report);
        return;
    }
    respond_text(fd, 0, OP_ERROR, "unknown admin operation");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int cfg_status = cfg_load(&g_cfg, argc, argv);
    if (cfg_status > 0) {
        return 0;
    }
    if (cfg_status < 0) {
        return 1;
    }

    if (mkdir("data", 0775) < 0 && errno != EEXIST) {
        perror("mkdir data");
        return 1;
    }

    g_state = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_state == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    state_init(g_state);
    queue_init(&g_queue);

    int listen_fd = listen_tcp(g_cfg.server_port);
    int admin_listen_fd = listen_tcp(g_cfg.admin_port);
    if (listen_fd < 0 || admin_listen_fd < 0) {
        return 1;
    }

    pthread_t worker;
    pthread_create(&worker, NULL, worker_main, NULL);

    struct pollfd fds[MAX_FDS];
    int nfds = 0;
    fds[nfds++] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
    fds[nfds++] = (struct pollfd){.fd = admin_listen_fd, .events = POLLIN};
    int admin_fd = -1;
    long admin_last = 0;

    fprintf(stderr,
            "sql-code-generator server listening on %u, admin on %u, timeout %ds, config %s\n",
            (unsigned)g_cfg.server_port, (unsigned)g_cfg.admin_port, g_cfg.admin_timeout_sec,
            g_cfg.config_path);

    for (;;) {
        int rc = poll(fds, nfds, 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (admin_fd > 0 && time(NULL) - admin_last > g_cfg.admin_timeout_sec) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd == admin_fd) {
                    remove_pollfd(fds, &nfds, i);
                    break;
                }
            }
            admin_fd = -1;
            g_state->admin_connected = 0;
            fprintf(stderr, "admin disconnected due to timeout\n");
        }

        for (int i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) {
                continue;
            }
            if (fds[i].fd == listen_fd) {
                accept_client(listen_fd, fds, &nfds);
                continue;
            }
            if (fds[i].fd == admin_listen_fd) {
                accept_admin(admin_listen_fd, fds, &nfds, &admin_fd, &admin_last);
                continue;
            }

            MsgHeader h;
            char *payload = NULL;
            int r = recv_message(fds[i].fd, &h, &payload);
            if (r <= 0) {
                if (fds[i].fd == admin_fd) {
                    admin_fd = -1;
                    g_state->admin_connected = 0;
                }
                free(payload);
                remove_pollfd(fds, &nfds, i);
                i--;
                continue;
            }

            if (fds[i].fd == admin_fd) {
                handle_admin_message(fds[i].fd, &admin_fd, &admin_last, &h, payload);
                if (admin_fd < 0) {
                    remove_pollfd(fds, &nfds, i);
                    i--;
                }
                free(payload);
                continue;
            }

            if (h.op_id == OP_CONNECT) {
                uint32_t id = (uint32_t)g_state->next_client_id++;
                char addr[64] = "client";
                register_client(fds[i].fd, addr, id);
                char msg[64];
                snprintf(msg, sizeof(msg), "%u", id);
                respond_text(fds[i].fd, id, OP_OK, msg);
                history_add("client connected");
                free(payload);
                continue;
            }

            if (h.op_id == OP_BYE) {
                respond_text(fds[i].fd, h.client_id, OP_OK, "bye");
                free(payload);
                remove_pollfd(fds, &nfds, i);
                i--;
                continue;
            }

            if (!is_registered_client(h.client_id)) {
                respond_text(fds[i].fd, h.client_id, OP_ERROR, "client must CONNECT first");
                free(payload);
                continue;
            }

            Request *req = (Request *)calloc(1, sizeof(*req));
            if (req == NULL) {
                respond_text(fds[i].fd, h.client_id, OP_ERROR, "server out of memory");
                free(payload);
                continue;
            }
            req->fd = fds[i].fd;
            req->client_id = h.client_id;
            req->op_id = h.op_id;
            req->payload = payload;
            req->payload_size = h.msg_size;
            queue_push(&g_queue, req);
        }
    }

    close(listen_fd);
    close(admin_listen_fd);
    return 0;
}

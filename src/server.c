/*
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * server.c
 * In acest fisier implementam serverul TCP al aplicatiei. Serverul accepta
 * clienti obisnuiti si un client admin, primeste fisiere JSON cu schema ER,
 * genereaza SQL, valideaza si aplica INSERT-uri si pastreaza statistici
 * despre comenzi, clienti si coada de procesare.
 */

#include "model.h"    /* modelul de date, validare INSERT si rapoarte admin */
#include "protocol.h" /* operatii si functii pentru protocolul TCP */

#include <arpa/inet.h>  /* inet_ntop si structuri pentru adrese IPv4 */
#include <errno.h>      /* errno si coduri de eroare */
#include <poll.h>       /* multiplexare I/O cu poll */
#include <pthread.h>    /* thread worker si sincronizare */
#include <signal.h>     /* ignorarea SIGPIPE */
#include <stdio.h>      /* functii standard de intrare/iesire */
#include <stdlib.h>     /* alocare dinamica si conversii */
#include <string.h>     /* manipulare siruri si memorie */
#include <sys/mman.h>   /* memorie partajata prin mmap */
#include <sys/socket.h> /* accept si structuri socket */
#include <sys/stat.h>   /* mkdir */
#include <sys/time.h>   /* gettimeofday pentru masurare timp */
#include <sys/types.h>  /* tipuri POSIX */
#include <sys/wait.h>   /* waitpid pentru procesul copil */
#include <time.h>       /* time pentru timestamp-uri */
#include <unistd.h>     /* close, pipe, fork, read, _exit */

#ifdef HAVE_LIBCONFIG
#include <libconfig.h>
#endif

/* numarul maxim de descriptori urmariti simultan prin poll */
#define MAX_FDS 256

/* timpul implicit dupa care conexiunea admin inactiva este inchisa */
#define DEFAULT_ADMIN_TIMEOUT_SEC 60

/*
 * configuratia efectiva a serverului dupa combinarea valorilor implicite,
 * fisierului de configurare, variabilelor de mediu si argumentelor CLI
 */
typedef struct {
    uint16_t server_port;      /* portul pentru clientii obisnuiti */
    uint16_t admin_port;       /* portul pentru clientul admin */
    int admin_timeout_sec;     /* limita de inactivitate pentru admin */
    char config_path[256];     /* calea fisierului de configurare */
} RuntimeConfig;

/*
 * o cerere primita de la un client obisnuit
 * cererile sunt puse in coada si procesate de thread-ul worker
 */
typedef struct Request {
    int fd;                  /* socketul clientului care asteapta raspuns */
    uint32_t client_id;      /* identificatorul logic al clientului */
    uint32_t op_id;          /* operatia ceruta */
    char *payload;           /* continutul mesajului primit */
    uint32_t payload_size;   /* dimensiunea payload-ului */
    struct Request *next;    /* urmatorul element din lista inlantuita */
} Request;

/*
 * coada thread-safe de cereri obisnuite
 * head/tail formeaza lista, iar mutex/cond sincronizeaza worker-ul
 */
typedef struct {
    Request *head;             /* primul element care va fi procesat */
    Request *tail;             /* ultimul element adaugat */
    pthread_mutex_t mutex;     /* protejeaza accesul la lista */
    pthread_cond_t cond;       /* trezeste worker-ul cand apare o cerere */
} RequestQueue;

static SharedState *g_state;  /* starea comuna a serverului */
static RequestQueue g_queue;  /* coada de cereri pentru worker */
static RuntimeConfig g_cfg;   /* configuratia runtime a serverului */

/*
 * afiseaza optiunile acceptate de server la pornire
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-c config.cfg] [-p server_port] [-a admin_port] [-t admin_timeout]\n",
            prog);
}

/*
 * parseaza un numar intreg din text si verifica daca se afla in intervalul cerut
 */
static int parse_number(const char *value, long min, long max, long *out) {
    char *end = NULL;
    errno = 0;

    /* strtol permite detectarea caracterelor invalide ramase la final */
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return -1;
    }
    *out = parsed;
    return 0;
}

/*
 * aplica o valoare de port citita din configuratie, mediu sau CLI
 * valorile invalide sunt ignorate si raportate pe stderr
 */
static void apply_port_value(const char *label, const char *value, uint16_t *target) {
    long parsed = 0;
    if (parse_number(value, 1, 65535, &parsed) == 0) {
        *target = (uint16_t)parsed;
    } else {
        fprintf(stderr, "Ignoring invalid %s value '%s'\n", label, value);
    }
}

/*
 * aplica valoarea timeout-ului admin
 * limita superioara evita valori exagerat de mari in configuratie
 */
static void apply_timeout_value(const char *label, const char *value, int *target) {
    long parsed = 0;
    if (parse_number(value, 1, 86400, &parsed) == 0) {
        *target = (int)parsed;
    } else {
        fprintf(stderr, "Ignoring invalid %s value '%s'\n", label, value);
    }
}

/*
 * seteaza valorile implicite ale configuratiei serverului
 */
static void cfg_defaults(RuntimeConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->server_port = SQLCG_PORT;
    cfg->admin_port = SQLCG_ADMIN_PORT;
    cfg->admin_timeout_sec = DEFAULT_ADMIN_TIMEOUT_SEC;
    snprintf(cfg->config_path, sizeof(cfg->config_path), "config.cfg");
}

/*
 * determina calea fisierului de configurare
 * prioritatea este: valoarea implicita, CONFIG_PATH, apoi -c/--config
 */
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

/*
 * incarca valorile din fisierul de configurare
 * daca libconfig este disponibil, folosim API-ul dedicat; altfel folosim
 * un parser simplu pentru liniile de forma cheie = valoare
 */
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

    /* citim campurile cunoscute din sectiunea server */
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
        /* cautam separatorul dintre cheie si valoare */
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        eq++;
        while (*eq == ' ' || *eq == '\t') {
            eq++;
        }

        /* extragem valoarea pana la comentariu, spatiu sau final de linie */
        char value[64];
        size_t len = strcspn(eq, "; \t\r\n");
        if (len == 0 || len >= sizeof(value)) {
            continue;
        }
        memcpy(value, eq, len);
        value[len] = '\0';

        /* alegem campul configurat dupa numele gasit pe linie */
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

/*
 * suprascrie configuratia folosind variabile de mediu
 */
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

/*
 * aplica argumentele din linia de comanda
 * acestea au prioritate peste valorile implicite, fisier si variabile de mediu
 */
static int cfg_apply_args(RuntimeConfig *cfg, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        /* -h/--help afiseaza utilizarea si opreste pornirea serverului */
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }

        /*
         * calea configuratiei a fost deja citita in cfg_read_config_path
         * aici doar sarim peste valoarea optiunii ca sa nu fie tratata ca eroare
         */
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            i++;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            /* suprascriem portul principal al serverului */
            const char *option = argv[i++];
            apply_port_value(option, argv[i], &cfg->server_port);
        } else if ((strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--admin-port") == 0) && i + 1 < argc) {
            /* suprascriem portul pentru clientul admin */
            const char *option = argv[i++];
            apply_port_value(option, argv[i], &cfg->admin_port);
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--admin-timeout") == 0) && i + 1 < argc) {
            /* suprascriem timeout-ul de inactivitate pentru admin */
            const char *option = argv[i++];
            apply_timeout_value(option, argv[i], &cfg->admin_timeout_sec);
        } else {
            /* orice optiune necunoscuta este raportata ca utilizare gresita */
            print_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/*
 * construieste configuratia finala a serverului in ordinea de prioritate dorita
 */
static int cfg_load(RuntimeConfig *cfg, int argc, char **argv) {
    cfg_defaults(cfg);
    cfg_read_config_path(cfg, argc, argv);
    cfg_load_file(cfg);
    cfg_apply_env(cfg);
    return cfg_apply_args(cfg, argc, argv);
}

/*
 * returneaza timpul curent in milisecunde
 * este folosit pentru masurarea duratei de procesare a unei comenzi
 */
static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

/*
 * adauga o linie in istoricul circular al serverului
 */
static void history_add(const char *line) {
    int idx = g_state->history_count % MAX_HISTORY;
    snprintf(g_state->history[idx], MAX_COMMAND, "%s", line);
    g_state->history_count++;
}

/*
 * initializeaza coada de cereri si mecanismele de sincronizare
 */
static void queue_init(RequestQueue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

/*
 * adauga o cerere la finalul cozii si trezeste thread-ul worker
 */
static void queue_push(RequestQueue *q, Request *r) {
    r->next = NULL;
    pthread_mutex_lock(&q->mutex);

    /* daca lista este goala, noul element devine si head, si tail */
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

/*
 * extrage prima cerere din coada
 * daca nu exista cereri, worker-ul asteapta pe variabila conditionala
 */
static Request *queue_pop(RequestQueue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    Request *r = q->head;
    q->head = r->next;

    /* cand am scos ultimul element, resetam si tail */
    if (q->head == NULL) {
        q->tail = NULL;
    }

    /* pastram statistica din SharedState sincronizata cu operatiile pe coada */
    if (g_state->queue_depth > 0) {
        g_state->queue_depth--;
    }
    pthread_mutex_unlock(&q->mutex);
    return r;
}

/*
 * actualizeaza momentul ultimei activitati pentru clientul dat
 */
static void client_touch(uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].client_id == (int)client_id && g_state->clients[i].fd > 0) {
            g_state->clients[i].last_seen = time(NULL);
            return;
        }
    }
}

/*
 * elimina din lista de clienti descriptorul inchis
 */
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

/*
 * trimite un raspuns text folosind operatia indicata
 */
static void respond_text(int fd, uint32_t client_id, uint32_t op, const char *text) {
    send_message(fd, client_id, op, text, (uint32_t)strlen(text));
}

/*
 * construieste calea fisierului temporar in care se salveaza upload-ul clientului
 */
static void upload_path(uint32_t client_id, char *out, size_t out_len) {
    snprintf(out, out_len, "data/upload_%u.schema.json", client_id);
}

/*
 * adauga un chunk primit de la client la fisierul temporar de upload
 */
static int append_upload_chunk(uint32_t client_id, const char *payload, uint32_t size, char *err, size_t err_len) {
    char path[128];

    /* fiecare client scrie intr-un fisier temporar separat */
    upload_path(client_id, path, sizeof(path));

    /* deschidem fisierul in mod append binar, deoarece upload-ul vine pe bucati */
    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        snprintf(err, err_len, "cannot append upload file: %s", strerror(errno));
        return -1;
    }

    /* scriem doar daca payload-ul are continut */
    if (size > 0 && fwrite(payload, 1, size, f) != size) {
        fclose(f);
        snprintf(err, err_len, "cannot write upload chunk");
        return -1;
    }

    /* inchidem fisierul dupa fiecare bucata pentru a nu tine descriptorul deschis */
    fclose(f);
    snprintf(err, err_len, "OK: chunk stored (%u bytes)", size);
    return 0;
}

/*
 * valideaza un INSERT intr-un proces copil
 * procesul copil trimite rezultatul inapoi catre parinte prin pipe
 */
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
        /* copilul nu citeste din pipe; doar scrie rezultatul validarii */
        close(pipefd[0]);
        char msg[1024];
        int ok = validate_insert_batch(g_state, sql, msg, sizeof(msg)) == 0;
        char packet[1200];
        snprintf(packet, sizeof(packet), "%d\n%s", ok ? 1 : 0, msg);
        write_full(pipefd[1], packet, strlen(packet));
        close(pipefd[1]);
        _exit(ok ? 0 : 1);
    }

    /* parintele citeste raspunsul copilului si asteapta terminarea lui */
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

/*
 * proceseaza o cerere obisnuita din coada si trimite raspunsul catre client
 */
static void process_request(Request *r) {
    /* memoram momentul de start pentru statistica de timp mediu */
    long start = now_ms();
    char response[65536];
    char err[2048];

    /* initializam buffer-ele ca siruri goale */
    response[0] = '\0';
    err[0] = '\0';
    uint32_t resp_op = OP_OK;

    /* marcam activitatea clientului inainte de procesarea cererii */
    client_touch(r->client_id);

    if (r->op_id == OP_UPLOAD_BEGIN) {
        /* pregatim fisierul temporar in care vor fi scrise chunk-urile JSON */
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
        /* salvam o bucata din fisierul incarcat */
        if (append_upload_chunk(r->client_id, r->payload, r->payload_size, response, sizeof(response)) < 0) {
            resp_op = OP_ERROR;
        }
    } else if (r->op_id == OP_UPLOAD_END) {
        /* dupa ultimul chunk, incercam sa incarcam schema ER din fisier */
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
        /* generam DDL pentru schema curenta */
        generate_sql(g_state, response, sizeof(response));
        history_add("generate sql");
    } else if (r->op_id == OP_VALIDATE_INSERT) {
        /* contorizam separat comenzile INSERT pentru raportul admin */
        g_state->insert_commands++;
        if (r->payload == NULL) {
            snprintf(response, sizeof(response), "empty insert payload");
            resp_op = OP_ERROR;
        } else if (child_validate_insert(r->payload, err, sizeof(err)) == 0) {
            /*
             * validarea izolata a trecut; aplicam batch-ul in procesul serverului
             * ca sa actualizam datele memorate in SharedState
             */
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
        /* operatiile necunoscute sunt respinse explicit */
        snprintf(response, sizeof(response), "unknown operation %u", r->op_id);
        resp_op = OP_ERROR;
    }

    /* actualizam statisticile comune pentru raportarea admin */
    long elapsed = now_ms() - start;
    g_state->total_commands++;
    g_state->total_exec_ms += elapsed;
    if (resp_op == OP_ERROR) {
        g_state->failed_commands++;
    }

    /* trimitem inapoi fie OP_OK, fie OP_ERROR, impreuna cu textul rezultat */
    respond_text(r->fd, r->client_id, resp_op, response);
}

/*
 * functia executata de thread-ul worker
 * asteapta cereri in coada, le proceseaza si elibereaza memoria aferenta
 */
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

/*
 * inchide si elimina un descriptor din vectorul folosit de poll
 */
static void remove_pollfd(struct pollfd *fds, int *nfds, int idx) {
    int fd = fds[idx].fd;
    close(fd);
    client_remove_fd(fd);

    /* mutam elementele din dreapta peste pozitia eliminata */
    if (idx < *nfds - 1) {
        memmove(&fds[idx], &fds[idx + 1], (size_t)(*nfds - idx - 1) * sizeof(struct pollfd));
    }
    (*nfds)--;
}

/*
 * inregistreaza un client obisnuit dupa handshake-ul OP_CONNECT
 */
static void register_client(int fd, const char *addr, uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        /* prima pozitie libera este indicata de fd == 0 */
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

    /* daca nu exista loc liber, clientul ramane neinitializat logic */
}

/*
 * accepta o conexiune noua pe portul clientilor obisnuiti
 * clientul va primi id-ul logic abia dupa mesajul OP_CONNECT
 */
static void accept_client(int listen_fd, struct pollfd *fds, int *nfds) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        /* la eroare de accept nu oprim serverul, doar ignoram conexiunea curenta */
        return;
    }
    if (*nfds >= MAX_FDS) {
        /* daca poll nu mai are loc, inchidem conexiunea noua */
        close(fd);
        return;
    }
    char ip[64];

    /* convertim adresa IPv4 in text pentru log si rapoarte */
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    fds[*nfds].fd = fd;
    fds[*nfds].events = POLLIN;
    fds[*nfds].revents = 0;
    (*nfds)++;
    fprintf(stderr, "ordinary client connected from %s\n", ip);
}

/*
 * accepta conexiunea admin
 * aplicatia permite un singur admin conectat simultan
 */
static void accept_admin(int listen_fd, struct pollfd *fds, int *nfds, int *admin_fd, long *admin_last) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
        /* la eroare de accept nu oprim serverul */
        return;
    }
    if (*admin_fd > 0) {
        /* refuzam al doilea admin pentru a evita comenzi administrative concurente */
        respond_text(fd, 0, OP_ERROR, "another admin client is already connected");
        close(fd);
        return;
    }
    if (*nfds >= MAX_FDS) {
        /* nu putem urmari mai multi descriptori decat limita locala */
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

/*
 * verifica daca un client_id apartine unui client conectat si activ
 */
static int is_registered_client(uint32_t client_id) {
    for (int i = 0; i < 128; i++) {
        if (g_state->clients[i].client_id == (int)client_id && g_state->clients[i].fd > 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * proceseaza mesajele primite pe conexiunea admin
 */
static void handle_admin_message(int fd, int *admin_fd, long *admin_last, MsgHeader *h, char *payload) {
    *admin_last = time(NULL);
    if (h->op_id == OP_ADMIN_BYE) {
        respond_text(fd, 0, OP_OK, "admin disconnected");
        *admin_fd = -1;
        g_state->admin_connected = 0;
        return;
    }
    if (h->op_id == OP_ADMIN_LOGIN) {
        /* autentificarea demonstrativa foloseste tokenul fix "admin" */
        if (payload != NULL && strcmp(payload, "admin") == 0) {
            respond_text(fd, 0, OP_OK, "OK: admin logged in");
        } else {
            respond_text(fd, 0, OP_ERROR, "invalid admin token");
        }
        return;
    }
    if (h->op_id == OP_ADMIN_REPORT) {
        /* categoria raportului este trimisa ca payload text */
        char report[65536];
        admin_report(g_state, payload != NULL ? payload : "", report, sizeof(report));
        respond_text(fd, 0, OP_OK, report);
        return;
    }
    respond_text(fd, 0, OP_ERROR, "unknown admin operation");
}

/*
 * punctul de intrare al serverului
 * initializeaza configuratia, socketurile, memoria partajata si bucla poll
 */
int main(int argc, char **argv) {
    /* evitam terminarea procesului daca scriem intr-un socket inchis */
    signal(SIGPIPE, SIG_IGN);

    /* incarcam configuratia finala a serverului */
    int cfg_status = cfg_load(&g_cfg, argc, argv);
    if (cfg_status > 0) {
        /* status pozitiv inseamna ca s-a cerut --help */
        return 0;
    }
    if (cfg_status < 0) {
        /* status negativ inseamna eroare in argumentele CLI */
        return 1;
    }

    /* directorul data este folosit pentru fisierele temporare incarcate de clienti */
    if (mkdir("data", 0775) < 0 && errno != EEXIST) {
        perror("mkdir data");
        return 1;
    }

    /*
     * SharedState este alocat in memorie partajata pentru ca procesul copil
     * creat la validarea INSERT-urilor sa poata vedea aceeasi stare
     */
    g_state = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_state == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    state_init(g_state);
    queue_init(&g_queue);

    /* deschidem cate un socket de ascultare pentru clienti si pentru admin */
    int listen_fd = listen_tcp(g_cfg.server_port);
    int admin_listen_fd = listen_tcp(g_cfg.admin_port);
    if (listen_fd < 0 || admin_listen_fd < 0) {
        /* daca unul dintre porturi nu poate fi deschis, serverul nu poate porni */
        return 1;
    }

    /* worker-ul proceseaza cererile obisnuite din coada */
    pthread_t worker;
    pthread_create(&worker, NULL, worker_main, NULL);

    /* vectorul poll porneste cu cele doua socketuri de ascultare */
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

        /* inchidem automat conexiunea admin daca depaseste timeout-ul de inactivitate */
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

        /* parcurgem toti descriptorii care au date de citit */
        for (int i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) {
                continue;
            }

            /* conexiune noua de client obisnuit */
            if (fds[i].fd == listen_fd) {
                accept_client(listen_fd, fds, &nfds);
                continue;
            }

            /* conexiune noua de admin */
            if (fds[i].fd == admin_listen_fd) {
                accept_admin(admin_listen_fd, fds, &nfds, &admin_fd, &admin_last);
                continue;
            }

            MsgHeader h;
            char *payload = NULL;
            int r = recv_message(fds[i].fd, &h, &payload);
            if (r <= 0) {
                /* conexiune inchisa sau eroare de citire */
                if (fds[i].fd == admin_fd) {
                    admin_fd = -1;
                    g_state->admin_connected = 0;
                }
                free(payload);
                remove_pollfd(fds, &nfds, i);
                i--;
                continue;
            }

            /* mesajele admin sunt procesate direct, fara coada worker */
            if (fds[i].fd == admin_fd) {
                handle_admin_message(fds[i].fd, &admin_fd, &admin_last, &h, payload);
                if (admin_fd < 0) {
                    remove_pollfd(fds, &nfds, i);
                    i--;
                }
                free(payload);
                continue;
            }

            /* primul mesaj al unui client obisnuit trebuie sa fie CONNECT */
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

            /* BYE inchide conexiunea clientului obisnuit */
            if (h.op_id == OP_BYE) {
                respond_text(fds[i].fd, h.client_id, OP_OK, "bye");
                free(payload);
                remove_pollfd(fds, &nfds, i);
                i--;
                continue;
            }

            /* orice alta operatie necesita un client_id inregistrat anterior */
            if (!is_registered_client(h.client_id)) {
                respond_text(fds[i].fd, h.client_id, OP_ERROR, "client must CONNECT first");
                free(payload);
                continue;
            }

            /* mesajul valid este impachetat ca Request si preluat de worker */
            Request *req = (Request *)calloc(1, sizeof(*req));
            if (req == NULL) {
                /* daca nu putem aloca cererea, raspundem imediat cu eroare */
                respond_text(fds[i].fd, h.client_id, OP_ERROR, "server out of memory");
                free(payload);
                continue;
            }

            /* transferam ownership-ul payload-ului catre Request */
            req->fd = fds[i].fd;
            req->client_id = h.client_id;
            req->op_id = h.op_id;
            req->payload = payload;
            req->payload_size = h.msg_size;

            /* de aici cererea va fi procesata asincron de worker */
            queue_push(&g_queue, req);
        }
    }

    /* inchidem socketurile de ascultare la iesirea din bucla principala */
    close(listen_fd);
    close(admin_listen_fd);
    return 0;
}

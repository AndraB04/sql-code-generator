/**
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * Client TCP principal
 * In acest fisier implementam clientul principal al aplicatiei SQL Code Generator.
 * Clientul se conecteaza la server prin TCP, poate incarca un fisier JSON
 * ce descrie diagrama ER, poate cere generarea instructiunilor SQL,
 * poate trimite batch-uri INSERT pentru validare si poate functiona
 * interactiv printr-o interfata de tip linie de comanda.
 */

#include "protocol.h"   /* definitii pentru protocolul de comunicatie si functiile TCP */

#include <errno.h>      /* variabila globala errno si coduri de eroare */
#include <stdio.h>      /* functii standard de intrare/iesire */
#include <stdlib.h>     /* conversii si alocare memorie */
#include <string.h>     /* manipulare siruri si memorie */
#include <unistd.h>     /* functii POSIX precum close */

/*
 * functie auxiliara care converteste un sir de caractere intr-un numar de port
 * valid din intervalul 1..65535
 */
static int parse_port(const char *value, uint16_t *port) {
    char *end = NULL;
    errno = 0;

    /* convertim textul in numar intreg */
    long parsed = strtol(value, &end, 10);

    /*
     * validam rezultatul:
     * - sa nu existe eroare la conversie
     * - sa se fi citit macar un caracter
     * - sa nu ramana caractere suplimentare la final
     * - portul sa fie in intervalul permis
     */
    if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
        return -1;
    }

    *port = (uint16_t)parsed;
    return 0;
}

/*
 * afiseaza modul de utilizare al programului din linia de comanda
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [host] [--host host] [--port port] [--input schema.json]\n"
            "          [--generate] [--insert-file batch.sql] [--no-repl]\n",
            prog);
}

/*
 * trimite o cerere text catre server si asteapta raspunsul
 * daca reply != NULL, raspunsul este returnat prin acel parametru
 * altfel raspunsul este afisat direct la consola
 */
static int request_text(int fd, uint32_t client_id, uint32_t op, const char *payload, char **reply) {
    /*
     * trimitem mesajul catre server folosind operatia specificata
     * payload-ul poate fi gol sau poate contine date text
     */
    if (send_message(fd, client_id, op, payload, payload ? (uint32_t)strlen(payload) : 0) < 0) {
        return -1;
    }

    MsgHeader h;      /* header-ul mesajului primit */
    char *resp = NULL;

    /* receptionam raspunsul serverului */
    int rc = recv_message(fd, &h, &resp);
    if (rc <= 0) {
        free(resp);
        return -1;
    }

    /*
     * daca apelantul vrea raspunsul inapoi, il lasam in reply
     * altfel il afisam imediat la consola si eliberam memoria
     */
    if (reply != NULL) {
        *reply = resp;
    } else {
        printf("%s\n", resp ? resp : "");
        free(resp);
    }

    /* succes doar daca serverul raspunde cu OP_OK */
    return h.op_id == OP_OK ? 0 : -1;
}

/*
 * realizeaza handshake-ul initial cu protocolul aplicatiei
 * serverul returneaza un client_id unic pentru clientul curent
 */
static int connect_protocol(int fd, uint32_t *client_id) {
    char *reply = NULL;

    /* trimitem cererea de conectare initiala */
    if (request_text(fd, 0, OP_CONNECT, "", &reply) < 0) {
        free(reply);
        return -1;
    }

    /* raspunsul serverului contine id-ul clientului sub forma text */
    *client_id = (uint32_t)strtoul(reply, NULL, 10);
    free(reply);

    return *client_id == 0 ? -1 : 0;
}

/*
 * incarca un fisier local si il trimite catre server in mai multe bucati
 * folosind operatiile OP_UPLOAD_BEGIN, OP_UPLOAD_CHUNK si OP_UPLOAD_END
 */
static int upload_file(int fd, uint32_t client_id, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /*
     * anuntam serverul ca urmeaza sa trimitem un fisier
     * payload-ul initial este chiar calea sau numele fisierului
     */
    if (request_text(fd, client_id, OP_UPLOAD_BEGIN, path, NULL) < 0) {
        fclose(f);
        return -1;
    }

    /* alocam bufferul pentru bucati de fisier */
    char *buf = (char *)malloc(SQLCG_FILE_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }

    /*
     * citim fisierul pe bucati si trimitem fiecare bloc catre server
     * dupa fiecare bloc asteptam confirmare OP_OK
     */
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

        /*
         * daca am citit mai putin decat dimensiunea maxima a blocului,
         * inseamna ca am ajuns la finalul fisierului sau a aparut o eroare
         */
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

    /* anuntam serverul ca upload-ul s-a incheiat */
    return request_text(fd, client_id, OP_UPLOAD_END, "", NULL);
}

/*
 * citeste complet un fisier text in memorie si returneaza un buffer alocat dinamic
 * apelantul este responsabil sa faca free() dupa utilizare
 */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }

    /* determinam dimensiunea fisierului */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);

    /*
     * fisierul nu trebuie sa fie negativ ca dimensiune si nici prea mare
     * fata de limita maxima acceptata de protocol
     */
    if (len < 0 || len > (long)SQLCG_MAX_PAYLOAD) {
        fclose(f);
        return NULL;
    }

    /* alocam memorie pentru continut + terminatorul de sir */
    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }

    /* citim tot fisierul in buffer */
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    buf[len] = '\0';
    return buf;
}

/*
 * afiseaza comenzile disponibile in modul interactiv
 */
static void print_help(void) {
    puts("Commands:");
    puts("  upload <schema.json>");
    puts("  generate");
    puts("  insert <INSERT INTO ...>");
    puts("  insertfile <batch.sql>");
    puts("  quit");
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";   /* host implicit */
    const char *input_path = NULL;    /* fisier JSON ER de incarcat optional */
    const char *insert_path = NULL;   /* fisier SQL cu INSERT-uri optional */
    uint16_t port = SQLCG_PORT;       /* portul implicit al serverului */

    int generate_once = 0;            /* daca s-a cerut generarea SQL din linia de comanda */
    int no_repl = 0;                  /* daca se dezactiveaza modul interactiv */

    /*
     * parcurgem argumentele din linia de comanda
     * si configuram clientul in functie de optiunile primite
     */
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
            /* permite specificarea directa a host-ului fara flag */
            host = argv[i];

        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    /* cream conexiunea TCP catre server */
    int fd = connect_tcp(host, port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:%u\n", host, (unsigned)port);
        return 1;
    }

    uint32_t client_id = 0;

    /* facem conectarea logica la protocol si obtinem id-ul clientului */
    if (connect_protocol(fd, &client_id) < 0) {
        fprintf(stderr, "protocol connect failed\n");
        close(fd);
        return 1;
    }

    printf("Connected as client %u\n", client_id);

    /*
     * daca s-a specificat un fisier de intrare JSON,
     * il incarcam imediat pe server
     */
    if (input_path != NULL && upload_file(fd, client_id, input_path) < 0) {
        close(fd);
        return 1;
    }

    /*
     * daca s-a cerut generarea SQL direct din linia de comanda,
     * trimitem cererea catre server
     */
    if (generate_once && request_text(fd, client_id, OP_GENERATE_SQL, "", NULL) < 0) {
        close(fd);
        return 1;
    }

    /*
     * daca s-a specificat un fisier cu INSERT-uri,
     * il citim si il trimitem pentru validare
     */
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

    /*
     * daca utilizatorul a cerut fara modul interactiv,
     * inchidem conexiunea elegant si terminam programul
     */
    if (no_repl) {
        request_text(fd, client_id, OP_BYE, "", NULL);
        close(fd);
        return 0;
    }

    /* afisam comenzile disponibile in modul interactiv */
    print_help();

    char line[8192];

    /*
     * bucla principala REPL:
     * citim comenzi de la tastatura si trimitem cereri corespunzatoare serverului
     */
    for (;;) {
        printf("sqlcg> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        /* eliminam caracterul newline citit de fgets */
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "upload ", 7) == 0) {
            /* comanda de upload a unui fisier JSON */
            upload_file(fd, client_id, line + 7);

        } else if (strcmp(line, "generate") == 0) {
            /* cerem generarea codului SQL */
            request_text(fd, client_id, OP_GENERATE_SQL, "", NULL);

        } else if (strncmp(line, "insert ", 7) == 0) {
            /* trimitem direct o instructiune INSERT pentru validare */
            request_text(fd, client_id, OP_VALIDATE_INSERT, line + 7, NULL);

        } else if (strncmp(line, "insertfile ", 11) == 0) {
            /* citim un fisier SQL si il trimitem pentru validare */
            char *sql = read_file(line + 11);
            if (sql == NULL) {
                fprintf(stderr, "cannot read insert file\n");
            } else {
                request_text(fd, client_id, OP_VALIDATE_INSERT, sql, NULL);
                free(sql);
            }

        } else if (strcmp(line, "help") == 0) {
            /* afisam din nou lista de comenzi */
            print_help();

        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            /* inchidem conexiunea cu serverul si iesim din bucla */
            request_text(fd, client_id, OP_BYE, "", NULL);
            break;

        } else if (line[0] != '\0') {
            /* pentru orice comanda necunoscuta, afisam ajutorul */
            print_help();
        }
    }

    /* inchidem descriptorul socketului */
    close(fd);
    return 0;
}


/*
./client          
Connected as client 1
Commands:
  upload <schema.json>
  generate
  insert <INSERT INTO ...>
  insertfile <batch.sql>
  quit
sqlcg> upload er_schema.json
cannot open er_schema.json: No such file or directory
sqlcg> upload examples/er_Schema.json
cannot open examples/er_Schema.json: No such file or directory
sqlcg> generate
-- No ER diagram loaded.

sqlcg> insertfile examples/er_schema.json
ERROR: no ER diagram loaded
sqlcg> inserfile examples/inserts_ok.sql
Commands:
  upload <schema.json>
  generate
  insert <INSERT INTO ...>
  insertfile <batch.sql>
  quit
sqlcg> insertfile examples/inserts_ok.sql
ERROR: constraint failed: duplicate value for departments.id
sqlcg> upload examples/er_schema.json
OK: upload started
OK: loaded 3 tables
sqlcg> genereate
Commands:
  upload <schema.json>
  generate
  insert <INSERT INTO ...>
  insertfile <batch.sql>
  quit
sqlcg> generate
CREATE TABLE departments (
    id INT PRIMARY KEY,
    name VARCHAR(80) NOT NULL UNIQUE
);

CREATE TABLE employees (
    id INT PRIMARY KEY,
    department_id INT NOT NULL REFERENCES departments(id),
    email VARCHAR(120) NOT NULL UNIQUE,
    full_name VARCHAR(120) NOT NULL
);

CREATE TABLE projects (
    id INT PRIMARY KEY,
    owner_id INT REFERENCES employees(id),
    code VARCHAR(30) NOT NULL UNIQUE
);


sqlcg> insertfile examples/inserts_ok.sql
OK: inserted 4 row(s)
sqlcg> insertfile examples/inserts_fail_fk.sql
ERROR: constraint failed: foreign key employees.department_id references missing departments.id=999
sqlcg> insertfile examples/inserts_fail_unique.sql
ERROR: constraint failed: duplicate value for departments.id
sqlcg> quit
bye

bash-5.3$ ./client --help
Usage: ./client [host] [--host host] [--port port] [--input schema.json]
          [--generate] [--insert-file batch.sql] [--no-repl]
bash-5.3$ ./client --host 127.0.0.1 --port 19081 --input examples/er_schema.json --generate --insert-file examples/inserts_ok.sql --no-repl
cannot connect to 127.0.0.1:19081
bash-5.3$ <^?./client --host 127.0.0.1 --port 18081 --input examples/er_schema.json --generate --insert-file examples/inserts_ok.sql --no-repl
                                                          
bash: ./client: No such file or directory
bash-5.3$ ./client --host 127.0.0.1 --port 18081 --input examples/er_schema.json --generate --insert-file examples/inserts_ok.sql --no-repl
Connected as client 4
OK: upload started
OK: loaded 3 tables
CREATE TABLE departments (
    id INT PRIMARY KEY,
    name VARCHAR(80) NOT NULL UNIQUE
);

CREATE TABLE employees (
    id INT PRIMARY KEY,
    department_id INT NOT NULL REFERENCES departments(id),
    email VARCHAR(120) NOT NULL UNIQUE,
    full_name VARCHAR(120) NOT NULL
);

CREATE TABLE projects (
    id INT PRIMARY KEY,
    owner_id INT REFERENCES employees(id),
    code VARCHAR(30) NOT NULL UNIQUE
);


OK: inserted 4 row(s)
bye
bash-5.3$ ./client 127.0.0.1 --input examples/er_schema.json --generate --insert-file examples/inserts_ok.sql --no-repl
Connected as client 5
OK: upload started
OK: loaded 3 tables
CREATE TABLE departments (
    id INT PRIMARY KEY,
    name VARCHAR(80) NOT NULL UNIQUE
);

CREATE TABLE employees (
    id INT PRIMARY KEY,
    department_id INT NOT NULL REFERENCES departments(id),
    email VARCHAR(120) NOT NULL UNIQUE,
    full_name VARCHAR(120) NOT NULL
);

CREATE TABLE projects (
    id INT PRIMARY KEY,
    owner_id INT REFERENCES employees(id),
    code VARCHAR(30) NOT NULL UNIQUE
);


OK: inserted 4 row(s)
bye
bash-5.3$ ./client --host 127.0.0.1 --port 18081 --insert-file examples/inserts_fail_fk.sql --no-repl
Connected as client 6
ERROR: constraint failed: foreign key employees.department_id references missing departments.id=999
bash-5.3$ ./client --host 127.0.0.1 --port 18081 --insert-file examples/inserts_fail_unique.sql --no-repl
Connected as client 7
ERROR: constraint failed: duplicate value for departments.id
bash-5.3$ 

*/
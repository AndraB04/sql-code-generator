/*
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * model.h
 * In acest fisier definim constantele, structurile si prototipurile
 * folosite pentru modelul de date al aplicatiei. Aceste definitii sunt
 * partajate intre server si functiile care incarca schema ER, genereaza
 * SQL, valideaza INSERT-uri si construiesc rapoarte administrative.
 */

#ifndef SQLCG_MODEL_H   /* daca macro-ul nu este definit, continuam */
#define SQLCG_MODEL_H   /* definim macro-ul pentru a evita includerea multipla */

/*
 * aceste directive se numesc "include guards"
 * ele previn includerea de mai multe ori a aceluiasi header
 * ceea ce ar duce la erori de compilare prin redefiniri
 */

#include <stddef.h> /* defineste tipul size_t folosit in prototipuri */

/* limitele maxime folosite pentru schema, date memorate si istoric */
#define MAX_TABLES 32
#define MAX_COLUMNS 32
#define MAX_ROWS 1024
#define MAX_NAME 64
#define MAX_TYPE 64
#define MAX_VALUE 128
#define MAX_HISTORY 128
#define MAX_COMMAND 128

/*
 * structura care descrie o coloana dintr-o tabela a modelului ER
 * campurile primary_key, unique si not_null sunt tratate ca valori booleene
 */
typedef struct {
    char name[MAX_NAME];        /* numele coloanei */
    char type[MAX_TYPE];        /* tipul SQL al coloanei */
    int primary_key;            /* 1 daca este cheie primara, 0 altfel */
    int unique;                 /* 1 daca valoarea trebuie sa fie unica */
    int not_null;               /* 1 daca valoarea nu poate fi NULL */
    char ref_table[MAX_NAME];   /* tabela referita pentru o cheie externa */
    char ref_column[MAX_NAME];  /* coloana referita pentru o cheie externa */
} Column;

/*
 * structura care memoreaza un rand inserat intr-o tabela
 * valorile sunt tinute ca text, iar NULL este memorat separat
 */
typedef struct {
    char values[MAX_COLUMNS][MAX_VALUE]; /* valorile coloanelor, indexate dupa pozitia coloanei */
    int is_null[MAX_COLUMNS];            /* 1 daca valoarea coloanei este NULL, 0 altfel */
} Row;

/*
 * structura care descrie o tabela incarcata din schema ER
 * contine atat definitia coloanelor, cat si randurile acceptate in memorie
 */
typedef struct {
    char name[MAX_NAME];              /* numele tabelei */
    int column_count;                 /* numarul de coloane definite */
    Column columns[MAX_COLUMNS];      /* vectorul de coloane */
    int row_count;                    /* numarul de randuri memorate */
    Row rows[MAX_ROWS];               /* vectorul de randuri acceptate */
} Table;

/*
 * informatii despre un client obisnuit conectat la server
 * sunt folosite pentru rapoartele admin si pentru validarea client_id
 */
typedef struct {
    int client_id;       /* identificatorul logic primit la conectare */
    int fd;              /* descriptorul socketului asociat clientului */
    char addr[64];       /* adresa clientului, sub forma text */
    long connected_at;   /* momentul conectarii */
    long last_seen;      /* momentul ultimei activitati */
} ClientInfo;

/*
 * starea comuna a serverului
 * este plasata in memorie partajata pentru ca procesul copil de validare
 * sa poata consulta aceeasi schema si aceleasi randuri deja acceptate
 */
typedef struct {
    long started_at;                         /* momentul pornirii serverului */
    long total_commands;                     /* numarul total de comenzi procesate */
    long insert_commands;                    /* numarul comenzilor INSERT primite */
    long failed_commands;                    /* numarul comenzilor terminate cu eroare */
    long total_exec_ms;                      /* timpul total de executie pentru comenzi */
    int queue_depth;                         /* cate cereri obisnuite asteapta in coada */
    int client_count;                        /* numarul de clienti obisnuiti activi */
    int next_client_id;                      /* urmatorul id alocat unui client */
    int admin_connected;                     /* 1 daca exista un client admin conectat */
    ClientInfo clients[128];                 /* lista clientilor cunoscuti */
    char history[MAX_HISTORY][MAX_COMMAND];  /* istoricul circular al comenzilor recente */
    int history_count;                       /* cate intrari au fost adaugate in istoric */
    int table_count;                         /* numarul de tabele incarcate */
    Table tables[MAX_TABLES];                /* tabelele modelului ER */
} SharedState;

/* initializeaza structura SharedState cu valorile implicite */
void state_init(SharedState *state);

/* incarca schema ER dintr-un fisier JSON si completeaza tabelele din state */
int load_er_json(SharedState *state, const char *path, char *err, size_t err_len);

/* genereaza instructiuni SQL CREATE TABLE pentru schema incarcata */
int generate_sql(const SharedState *state, char *out, size_t out_len);

/* valideaza un batch de instructiuni INSERT fara sa modifice datele memorate */
int validate_insert_batch(const SharedState *state, const char *sql, char *err, size_t err_len);

/* valideaza si aplica un batch de INSERT-uri in tabelele din state */
int apply_insert_batch(SharedState *state, const char *sql, char *err, size_t err_len);

/* construieste un raport administrativ pentru categoria ceruta */
void admin_report(const SharedState *state, const char *category, char *out, size_t out_len);

#endif

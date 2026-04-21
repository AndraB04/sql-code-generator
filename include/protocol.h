/**
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * protocol.h
 * In acest fisier definim constantele, codurile operatiilor,
 * structura antetului de mesaj si prototipurile functiilor
 * folosite pentru comunicatia TCP dintre client si server
 */

#ifndef SQLCG_PROTOCOL_H   /* daca macro-ul nu este definit, continuam */
#define SQLCG_PROTOCOL_H   /* definim macro-ul pentru a evita includerea multipla */

/* 
 * aceste directive se numesc "include guards"
 * ele previn includerea de mai multe ori a aceluiasi header
 * ceea ce ar duce la erori de compilare (redefiniri)
 */

#include <stdint.h> /* tipuri intregi cu dimensiune fixa: uint16_t, uint32_t */
#include <stddef.h> /* defineste tipul size_t */

/* portul principal folosit pentru comunicatia client-server */
#define SQLCG_PORT 18081

/* portul folosit pentru conexiunea de administrare */
#define SQLCG_ADMIN_PORT 18082

/* dimensiunea maxima acceptata pentru payload-ul unui mesaj */
#define SQLCG_MAX_PAYLOAD (1024u * 1024u)

/* dimensiunea unui bloc de date transmis la upload de fisier */
#define SQLCG_FILE_CHUNK (64u * 1024u)

/* enumerare cu identificatorii operatiilor suportate de protocol */
enum {
    OP_CONNECT = 0,          /* cerere de conectare initiala */
    OP_BYE = 5,              /* cerere de inchidere a conexiunii */

    OP_UPLOAD_BEGIN = 10,    /* inceputul operatiei de upload */
    OP_UPLOAD_CHUNK = 11,    /* trimiterea unui bloc din fisier */
    OP_UPLOAD_END = 12,      /* finalizarea operatiei de upload */

    OP_GENERATE_SQL = 20,    /* cerere pentru generarea codului SQL */
    OP_VALIDATE_INSERT = 30, /* cerere pentru validarea unei instructiuni INSERT */

    OP_OK = 40,              /* raspuns care indica succes */
    OP_ERROR = 41,           /* raspuns care indica eroare */

    OP_ADMIN_LOGIN = 100,    /* autentificare in modul admin */
    OP_ADMIN_REPORT = 101,   /* cerere pentru raport/statistici admin */
    OP_ADMIN_BYE = 105       /* inchiderea conexiunii admin */
};

/* structura care reprezinta antetul unui mesaj transmis in protocol */
typedef struct {
    uint32_t msg_size;   /* dimensiunea payload-ului mesajului */
    uint32_t client_id;  /* identificatorul clientului */
    uint32_t op_id;      /* codul operatiei solicitate sau raspunsului */
    uint32_t flags;      /* camp pentru optiuni sau extensii ulterioare */
} MsgHeader;

/* realizeaza o conexiune TCP catre host-ul si portul specificat */
int connect_tcp(const char *host, uint16_t port);

/* creeaza un socket TCP de ascultare pe portul specificat */
int listen_tcp(uint16_t port);

/* citeste exact len octeti din descriptorul fd in buffer */
int read_full(int fd, void *buf, size_t len);

/* scrie exact len octeti din buffer catre descriptorul fd */
int write_full(int fd, const void *buf, size_t len);

/* trimite un mesaj complet: header + payload */
int send_message(int fd, uint32_t client_id, uint32_t op_id, const void *payload, uint32_t size);

/* receptioneaza un mesaj complet si aloca memorie pentru payload */
int recv_message(int fd, MsgHeader *header, char **payload);

#endif

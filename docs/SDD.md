# Software Design Description

## 1. Arhitectura

Sistemul are trei executabile:

- `server`: proces UNIX C care mentine starea si proceseaza cereri.
- `client`: client ordinar CLI pentru upload, generare SQL si inserturi.
- `admin`: client ncurses pentru rapoarte administrative.

Serverul foloseste doua socket-uri TCP. Socket-ul ordinar si socket-ul admin sunt multiplexate cu `poll`. Cererile clientilor ordinari sunt puse intr-o coada FIFO comuna. Un thread worker scoate cererile din coada si raspunde sincron clientului.

## 2. Stare partajata

Structura `SharedState` este alocata cu `mmap(..., MAP_SHARED | MAP_ANONYMOUS, ...)`. Ea contine:

- lista de tabele si coloane;
- randurile acceptate pentru validare ulterioara;
- clienti conectati;
- contori de comenzi;
- istoric circular;
- adancimea cozii;
- indicatorul de admin conectat.

Procesele copil create pentru inserturi citesc aceeasi stare partajata. Ele nu modifica starea. Daca validarea este reusita, parintele aplica batch-ul in `SharedState`.

## 3. Protocol

Header-ul este fix:

```c
typedef struct {
    uint32_t msg_size;
    uint32_t client_id;
    uint32_t op_id;
    uint32_t flags;
} MsgHeader;
```

Campurile sunt trimise in network byte order. Payload-ul poate fi text sau bytes de fisier. Operatii principale:

- `OP_CONNECT`: aloca id client.
- `OP_UPLOAD_BEGIN`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_END`: transfer ER.
- `OP_GENERATE_SQL`: returneaza SQL generat.
- `OP_VALIDATE_INSERT`: valideaza si aplica batch insert.
- `OP_ADMIN_LOGIN`, `OP_ADMIN_REPORT`, `OP_ADMIN_BYE`: administrare.

## 4. Validare inserturi

Parserul accepta:

```sql
INSERT INTO table (col1, col2) VALUES (v1, v2), (v3, v4);
```

Pentru fiecare rand se verifica:

- tabela exista;
- coloanele exista;
- coloanele `NOT NULL` si `PRIMARY KEY` nu primesc `NULL`;
- valorile `PRIMARY KEY` si `UNIQUE` nu exista deja si nu se repeta in acelasi batch;
- foreign key-ul indica o valoare existenta in tabela referita sau intr-un rand validat anterior din acelasi batch.

## 5. Administrare

Clientul admin este sincron si exclusiv. Serverul refuza al doilea admin cat timp primul este conectat. Daca adminul nu trimite comenzi timp de 60 de secunde, conexiunea este inchisa.

Rapoartele implementate sunt:

- clienti conectati;
- comenzi totale, inserturi si esecuri;
- durata medie de executie;
- istoric recent;
- tabele incarcate si numar de randuri;
- adancimea cozii.

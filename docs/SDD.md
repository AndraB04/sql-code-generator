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

### Format payload

- Encoding: UTF-8 pentru text
- Fisierele sunt transmise chunked (bytes)
- Dimensiunea este data de `msg_size`
- Nu exista delimitatori suplimentari

### Coduri flags

- 0 = succes
- 1 = eroare validare
- 2 = eroare protocol
- 3 = eroare interna

### Exemple mesaje

#### OP_CONNECT

```C
Request:
msg_size = 0
client_id = 0
op_id = OP_CONNECT
flags = 0

Response:
msg_size = 0
client_id = 42
op_id = OP_CONNECT
flags = 0
```

#### OP_UPLOAD_CHUNK

```c
Request:
msg_size = 512
client_id = 42
op_id = OP_UPLOAD_CHUNK
flags = 0

Payload:
"bytes JSON"
```

#### OP_GENERATE_SQL

```c
Response:
msg_size = N
client_id = 42
op_id = OP_GENERATE_SQL
flags = 0

Payload:
"CREATE TABLE users (...);"
```
## 4. Fluxuri (FCE)

### Upload ER

1. Client trimite OP_UPLOAD_BEGIN  
2. Server initializeaza buffer  
3. Client trimite OP_UPLOAD_CHUNK (de mai multe ori)  
4. Client trimite OP_UPLOAD_END  
5. Server parseaza JSON si actualizeaza starea  
6. Server trimite raspuns  

### Generare SQL

1. Client trimite OP_GENERATE_SQL  
2. Server genereaza SQL  
3. Server trimite rezultatul  

### Insert

1. Client trimite OP_VALIDATE_INSERT  
2. Server pune cererea in coada FIFO  
3. Worker thread extrage cererea  
4. Server apeleaza fork()  
5. Procesul copil valideaza  
6. Parintele asteapta cu waitpid()  
7. Daca valid → aplica in state  
8. Server raspunde clientului  

## 5. Validare inserturi

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

## 6. Procese

Serverul foloseste `fork()` pentru validarea inserturilor si `waitpid()` pentru sincronizare.

Procesele copil:
- citesc starea partajata
- nu modifica starea

Procesul parinte:
- asteapta finalizarea copilului
- aplica modificarile daca validarea reuseste

## 7. Configurare (libconfig)

Serverul foloseste `libconfig` pentru:

- configurarea porturilor
- limitele sistemului
- timeout-ul conexiunii admin

## 8. Administrare

Clientul admin este sincron si exclusiv. Serverul refuza al doilea admin cat timp primul este conectat. Daca adminul nu trimite comenzi timp de 60 de secunde, conexiunea este inchisa.

Rapoartele implementate sunt:

- clienti conectati;
- comenzi totale, inserturi si esecuri;
- durata medie de executie;
- istoric recent;
- tabele incarcate si numar de randuri;
- adancimea cozii.

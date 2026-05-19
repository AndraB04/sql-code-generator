# Software Design Description - Milestone 2

## Arhitectura

Sistemul este impartit in cinci componente:

- `server`: proces TCP principal, cu doua porturi: IN pentru clienti ordinari si UX pentru admin.
- `client`: client ordinar C pentru upload schema, generare SQL, download SQL si validare INSERT.
- `admin`: client C/ncurses pentru rapoarte si operatii administrative.
- `clients/python_client.py`: client ordinar alternativ in Python pentru demonstrarea interoperabilitatii protocolului.
- `model`: modul comun pentru schema ER, generare SQL, validare si rapoarte; foloseste `cJSON` si `libpg_query`.

## Protocol

Fiecare mesaj are un header fix `MsgHeader` in network byte order:

```c
uint32_t msg_size;
uint32_t client_id;
uint32_t op_id;
uint32_t flags;
```

Payload-ul este binar si are exact `msg_size` octeti. Operatiile principale sunt:

- `OP_CONNECT`, `OP_BYE`
- `OP_UPLOAD_BEGIN`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_END`
- `OP_DOWNLOAD_SQL`, `OP_DOWNLOAD_BEGIN`, `OP_DOWNLOAD_CHUNK`, `OP_DOWNLOAD_END`
- `OP_GENERATE_SQL`, `OP_VALIDATE_INSERT`
- `OP_JOB_STATUS`, `OP_JOB_RESULT`
- `OP_ADMIN_LOGIN`, `OP_ADMIN_REPORT`, `OP_ADMIN_DISCONNECT`, `OP_ADMIN_CANCEL`, `OP_ADMIN_BLOCK`, `OP_ADMIN_BYE`

## Server

Serverul foloseste doua fire de executie I/O. Firul IN gestioneaza clientii ordinari prin socket INET si `poll()`. Firul UX gestioneaza clientul de administrare prin socket separat si `poll()`. Clientii ordinari trebuie sa trimita `OP_CONNECT` inaintea altor operatii. Dupa conectare primesc un `client_id` unic.

Operatiile clientilor ordinari sunt impachetate in `Request` si puse intr-o coada FIFO protejata de `pthread_mutex_t` si `pthread_cond_t`. Un worker proceseaza cererile si trimite raspunsul catre client.

Clientul admin este acceptat pe un port separat. Exista un singur admin conectat la un moment dat. Daca adminul este inactiv peste timeout, serverul inchide conexiunea si permite conectarea altui admin.

Fiecare operatie de procesare primeste `job_id` si este retinuta in `SharedState` cu status `QUEUED`, `RUNNING`, `DONE` sau `ERROR`. Clientul poate cere sincron `OP_JOB_STATUS` sau `OP_JOB_RESULT`. Evenimentele principale sunt scrise in `logs/server.log`.

## Transfer Fisiere

Upload-ul foloseste fisiere temporare `data/upload_<client_id>.schema.json`. Clientul trimite:

1. `OP_UPLOAD_BEGIN`
2. mai multe mesaje `OP_UPLOAD_CHUNK`
3. `OP_UPLOAD_END`

Download-ul SQL generat foloseste:

1. clientul trimite `OP_DOWNLOAD_SQL`
2. serverul trimite `OP_DOWNLOAD_BEGIN`
3. serverul trimite mai multe `OP_DOWNLOAD_CHUNK`
4. serverul trimite `OP_DOWNLOAD_END`

Dimensiunea chunk-ului este `SQLCG_FILE_CHUNK`, adica 64 KB.

## Model Date

`SharedState` retine schema incarcata, randurile acceptate, clientii activi si statisticile administrative. Modulul `model.c` incarca JSON-ul ER cu `cJSON`, genereaza SQL si valideaza batch-uri `INSERT`.

Pentru fiecare batch `INSERT`, serverul creeaza un proces copil cu `fork()`. Copilul valideaza batch-ul folosind aceeasi stare `SharedState` mapata prin `mmap()`, apoi trimite rezultatul catre parinte prin pipe. Parintele aplica batch-ul doar daca validarea copilului a reusit.

Validarea SQL are doua straturi:

- `libpg_query` verifica sintaxa PostgreSQL a query-ului;
- validatorul semantic intern verifica modelul ER incarcat si anticipeaza erorile `constraint failed`.

Validarea verifica:

- existenta tabelei si a coloanelor;
- `NOT NULL`;
- duplicate pentru `PRIMARY KEY` si `UNIQUE`;
- foreign key catre randuri existente;
- consistenta numarului de coloane si valori.

## Operatii Admin

Rapoartele sunt generate de `admin_report()`. Operatiile active sunt procesate direct in server:

- deconectare client: cauta `client_id`, notifica socketul clientului si il elimina din `poll`;
- anulare comanda: marcheaza `cancel_client_id`, iar workerul respinge cererea urmatoare/curenta observabila pentru acel client;
- blocare IP/domeniu: adauga adresa intr-o blocklist si refuza conexiunile IN viitoare de la acea adresa.

## Decizii Pentru Milestone 2

- Coada de procesare este inclusa pentru toate cererile clientilor ordinari.
- Transferul este bidirectional pentru punctajul maxim pe criteriul de fisiere.
- Clientul admin are atat UI ncurses, cat si mod neinteractiv pentru testare.
- Clientul Python foloseste acelasi protocol binar si poate fi rulat de pe alt sistem.
- Fisierele locale din client folosesc apeluri POSIX (`open`, `read`, `write`, `close`) pentru zona IN.
- Serverul proceseaza datele in C si nu porneste scripturi externe Python/Java.
- Componenta web services nu este implementata in Milestone 2; daca se adauga ulterior, directia proiectata este gSOAP/SOAP.

## Analiza Statica Si Profilare

Buildul implicit foloseste `-Wall -Wextra -Wpedantic -Werror -std=c11 -D_POSIX_C_SOURCE=200809L`. Serverul este linkat cu `-pthread`.

Repository-ul include `.clang-tidy` cu checks pentru `clang-analyzer`, `bugprone`, `cert`, `concurrency`, `misc`, `performance`, `portability` si `readability`, cu `WarningsAsErrors: '*'`.

Makefile-ul expune tintele:

- `make clang-tidy`: analiza statica pe toate sursele C;
- `make asan`: Address/Leak/Undefined Sanitizer;
- `make tsan`: Thread Sanitizer;
- `make memcheck`: Valgrind Memcheck;
- `make helgrind`: Valgrind Helgrind.

Implementarea evita `strcpy`, `strcat`, `system`, `exec`, `popen`, `vfork`, VLA si `pthread_cancel`. I/O-ul pe socketuri si fisiere foloseste wrappers cu verificarea valorilor returnate.

## Limitari Cunoscute

- Schema ER este parsata cu `cJSON`, dar formatul acceptat ramane cel documentat in `examples/er_schema.json`.
- Sintaxa SQL este validata cu `libpg_query`; validatorul semantic acopera `INSERT INTO table [(cols...)] VALUES (...), (...);`.
- Anularea admin este cooperativa la nivelul workerului; pentru operatii foarte scurte efectul poate fi observat doar ca cerere de anulare in raport.

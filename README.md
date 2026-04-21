# SQL Code Generator

Proiect PCD pentru Milestone 1: aplicatie client-server in C care primeste o schema ER in JSON, genereaza SQL `CREATE TABLE` si valideaza preventiv batch-uri `INSERT` inainte ca acestea sa ajunga intr-o baza de date reala.

Ideea centrala este sa detecteze din timp erori precum tabel inexistent, coloana inexistenta, `NOT NULL`, duplicate pe `PRIMARY KEY`/`UNIQUE` si foreign key lipsa. Serverul pastreaza o stare demonstrativa in memorie partajata, foloseste o coada FIFO pentru cererile clientilor si valideaza inserturile intr-un proces copil creat cu `fork()`.

Executabile:

- `server`: proces UNIX care asculta clienti ordinari si un canal separat de administrare.
- `client`: CLI pentru upload schema ER, generare SQL si validare inserturi.
- `admin`: client ncurses pentru rapoarte despre clienti, comenzi, istoric, tabele si coada.

## Build

```sh
make
```

Dependinte de sistem:

- compilator C si pthread;
- `libconfig` pentru configurare (`libconfig-dev` pe Debian/Ubuntu);
- `ncurses` pentru clientul admin (`libncurses-dev` pe Debian/Ubuntu).

Makefile-ul detecteaza libconfig prin `pkg-config` si activeaza integrarea cu `-DHAVE_LIBCONFIG`. Daca headerul nu este disponibil in mediul local, serverul ramane compilabil cu un parser de fallback pentru acelasi format `config.cfg`; pentru evaluarea milestone-ului trebuie instalat `libconfig-dev`.

## Rulare

Demo neinteractiv:

```sh
./server
./client --input examples/er_schema.json --generate --insert-file examples/inserts_ok.sql --no-repl
```

Demo interactiv:

Terminal 1:

```sh
./server
```

Terminal 2:

```sh
./client 127.0.0.1
upload examples/er_schema.json
generate
insertfile examples/inserts_ok.sql
insertfile examples/inserts_fail_fk.sql
quit
```

Terminal 3:

```sh
./admin 127.0.0.1
```

Clientul admin foloseste tastele `1..6` pentru rapoarte: clienti, comenzi, durata medie, istoric, tabele si coada.

## Configurare

Serverul citeste configuratia in ordinea:

1. valori implicite: `18081`, `18082`, timeout admin `60s`;
2. fisier `config.cfg`, prin libconfig;
3. variabile de mediu: `CONFIG_PATH`, `SERVER_PORT`, `ADMIN_PORT`, `ADMIN_TIMEOUT`;
4. argumente CLI: `--config`, `--port`, `--admin-port`, `--admin-timeout`.

Exemple:

```sh
SERVER_PORT=19081 ADMIN_PORT=19082 ./server
./server --config config.cfg --port 19081 --admin-port 19082
./client --host 127.0.0.1 --port 19081 --input examples/er_schema.json --generate --no-repl
./admin --host 127.0.0.1 --port 19082
```

## Protocol

Protocolul pastreaza ideea din skeleton: fiecare mesaj are header fix cu `msg_size`, `client_id`, `op_id`, `flags`, urmat de payload binar. Transferul de fisiere este chunked prin `OP_UPLOAD_BEGIN`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_END`, cu bucati de 64 KB.

Documentele pentru Milestone 1 sunt in:

- `docs/SRS.md`: cerinte, fluxuri FCE si mapare pe livrabile;
- `docs/SDD.md`: arhitectura si decizii de proiectare;
- `docs/openapi.yaml`: specificatie OpenAPI pentru interfata HTTP/WS planificata peste protocolul binar existent.

## Observatii

Parserul ER accepta formatul JSON din `examples/er_schema.json`. Parserul SQL acopera `INSERT INTO table [(cols...)] VALUES (...), (...);`, suficient pentru validarea de chei primare, `UNIQUE`, `NOT NULL` si foreign key in demo.

# SQL Code Generator

Proiect PCD pentru Milestone 2: aplicatie client-server care primeste o schema ER in JSON, genereaza cod SQL `CREATE TABLE`, valideaza preventiv batch-uri `INSERT` si ofera administrare separata pentru server.

Serverul proceseaza cererile clientilor ordinari prin socket INET, pune operatiile de procesare intr-o coada FIFO unica si ruleaza validarea INSERT in procese copil. Clientul de administrare ruleaza pe un canal separat, permite un singur admin conectat simultan si este deconectat automat dupa timeout.

Executabile:

- `server`: server UNIX/C cu fire separate pentru clienti ordinari, admin si worker FIFO.
- `client`: client ordinar C pentru upload schema, generare SQL, download SQL si validare INSERT.
- `admin`: client de administrare ncurses pentru rapoarte si operatii de mentenanta.
- `clients/python_client.py`: client ordinar alternativ in Python, pe acelasi protocol INET.

## Cerinte

Dependinte de sistem:

- `gcc`, `make`, `pthread`;
- `libconfig-dev`;
- `libncurses-dev`;
- `libcjson-dev`;
- `libpg-query-dev`;
- optional pentru verificari: `clang-tidy`, `valgrind`, `pandoc`.

Proiectul poate folosi bibliotecile instalate global sau pachetele extrase local in `third_party/local`. In varianta curenta, `Makefile` include automat si aceasta cale locala pentru `cJSON` si `libpg_query`.

## Build

```sh
make
```

Executabilele rezultate sunt create in radacina proiectului:

```sh
./server
./client
./admin
```

Buildul foloseste flag-urile stricte cerute:

```sh
gcc -g -Wall -Wextra -Wpedantic -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
```

Verificari utile inainte de prezentare:

```sh
make clang-tidy
make asan
make tsan
make memcheck
make helgrind
make docs
```

`make memcheck` porneste serverul sub Valgrind. `make asan` si `make tsan` compileaza variante cu sanitizere. `make docs` genereaza PDF-urile pentru `docs/SRS.md` si `docs/SDD.md`.

## Pornire Rapida

Terminal 1, porneste serverul:

```sh
./server
```

Terminal 2, ruleaza clientul ordinar C:

```sh
./client 127.0.0.1
```

In consola clientului:

```text
upload examples/er_schema.json
generate
status
result
download /tmp/generated.sql
insertfile examples/inserts_ok.sql
status
result
insertfile examples/inserts_fail_fk.sql
quit
```

Ce demonstreaza comenzile:

- `upload`: transfer client -> server al diagramei ER JSON.
- `generate`: genereaza cod SQL `CREATE TABLE` pe server.
- `download`: transfer server -> client al SQL-ului generat.
- `insertfile`: trimite un batch de INSERT-uri pentru validare.
- `status`: cere sincron starea ultimului job sau a unui job explicit.
- `result`: cere sincron rezultatul ultimului job sau al unui job explicit.

Pentru un job anume:

```text
status 3
result 3
```

## Demo Neinteractiv

Porneste serverul intr-un terminal:

```sh
./server
```

Ruleaza un scenariu complet in alt terminal:

```sh
./client --host 127.0.0.1 --input examples/er_schema.json --generate --download-sql /tmp/generated.sql --insert-file examples/inserts_ok.sql --no-repl
```

Scenariu cu eroare de constrangere:

```sh
./client --host 127.0.0.1 --input examples/er_schema.json --insert-file examples/inserts_fail_fk.sql --no-repl
```

Scenariu cu clientul Python alternativ:

```sh
python3 clients/python_client.py --host 127.0.0.1 --input examples/er_schema.json --generate --download-sql /tmp/generated_py.sql --insert-file examples/inserts_ok.sql --status last --result last
```

## Client Admin

Pornire interactiva:

```sh
./admin 127.0.0.1
```

Taste disponibile in interfata ncurses:

- `0`: joburi de procesare.
- `1`: clienti conectati.
- `2`: comenzi in curs.
- `3`: durata medie de executie.
- `4`: istoric.
- `5`: tabele incarcate.
- `6`: coada FIFO.
- `7`: deconectare client ordinar.
- `8`: anulare comanda pentru un client.
- `9`: blocare IP/domeniu.
- `q`: iesire.

Adminul se poate folosi si fara ncurses, util pentru demo automat:

```sh
./admin --host 127.0.0.1 --report clients
./admin --host 127.0.0.1 --report jobs
./admin --host 127.0.0.1 --report queue
./admin --host 127.0.0.1 --disconnect 3
./admin --host 127.0.0.1 --cancel 3
./admin --host 127.0.0.1 --block 192.0.2.10
```

Restrictia 1:1 pentru admin este implementata: cat timp un admin este conectat, alt client admin primeste eroare. Daca adminul ramane inactiv peste timeout, serverul il deconecteaza.

## Configurare

Valorile implicite sunt:

- port clienti ordinari: `18081`;
- port admin: `18082`;
- timeout admin: `60` secunde.

Serverul citeste configuratia in ordinea:

1. valori implicite;
2. fisier `config.cfg`, prin libconfig;
3. variabile de mediu: `CONFIG_PATH`, `SERVER_PORT`, `ADMIN_PORT`, `ADMIN_TIMEOUT`;
4. argumente CLI: `--config`, `--port`, `--admin-port`, `--admin-timeout`.

Exemple:

```sh
SERVER_PORT=19081 ADMIN_PORT=19082 ./server
./server --config config.cfg --port 19081 --admin-port 19082 --admin-timeout 30
./client --host 127.0.0.1 --port 19081
./admin --host 127.0.0.1 --port 19082
```

## Protocol Si Operatii

Protocolul foloseste un header binar fix cu `msg_size`, `client_id`, `op_id`, `flags`, urmat de payload.

Operatii principale pentru clientul ordinar:

- conectare si deconectare;
- upload schema ER JSON;
- generare SQL;
- download SQL generat;
- validare batch INSERT;
- verificare status job;
- citire rezultat job.

Transferul de fisiere este bidirectional:

- client -> server: `OP_UPLOAD_BEGIN`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_END`;
- server -> client: `OP_DOWNLOAD_SQL`, `OP_DOWNLOAD_BEGIN`, `OP_DOWNLOAD_CHUNK`, `OP_DOWNLOAD_END`.

Operatiile de procesare primesc `job_id` si sunt puse in coada FIFO unica. Joburile au stari de tip `QUEUED`, `RUNNING`, `DONE`, `ERROR`.

## Validare SQL

Serverul proceseaza datele in C, fara proxy catre Python/Java:

- `cJSON` parseaza diagrama ER din `examples/er_schema.json`;
- `libpg_query` valideaza sintactic query-urile SQL;
- validarea semantica anticipeaza erori de tip `constraint failed`.

Sunt verificate:

- tabel inexistent;
- coloana inexistenta;
- `NOT NULL`;
- duplicate pe `PRIMARY KEY`;
- duplicate pe `UNIQUE`;
- foreign key lipsa.

Exemple:

```sh
./client --host 127.0.0.1 --input examples/er_schema.json --insert-file examples/inserts_ok.sql --no-repl
./client --host 127.0.0.1 --input examples/er_schema.json --insert-file examples/inserts_fail_fk.sql --no-repl
./client --host 127.0.0.1 --input examples/er_schema.json --insert-file examples/inserts_fail_unique.sql --no-repl
```

## Logging

Serverul scrie evenimentele principale in:

```text
logs/server.log
```

Fisierul contine pornirea serverului, porturile pe care asculta, clienti acceptati, joburi puse in coada, schimbari de stare si deconectari.

## Documentatie

Documentele pentru Milestone 2 sunt:

- `docs/SRS.md`: SRS actualizat dupa Milestone 1;
- `docs/SDD.md`: versiune preliminara SDD, corelata cu implementarea.

PDF-urile se regenereaza cu:

```sh
make docs
```

## Observatii Pentru Cerinte

- Serverul nu executa scripturi Python/Java pentru procesare. Clientul Python este doar un client ordinar alternativ.
- Clientii ordinari sunt acceptati prin socket INET si sunt gestionati cu `poll()`.
- Exista fire separate pentru interfata IN, interfata admin si workerul de procesare.
- Pentru fiecare batch de INSERT se creeaza proces copil prin `fork()`, iar rezultatul revine prin pipe anonim.
- Starea interna folosita la validare este tinuta in memorie partajata.
- Componenta web services nu este implementata in Milestone 2. Daca va fi adaugata, directia ceruta este gSOAP/SOAP, nu REST/OpenAPI.

# SQL Code Generator

Implementare C pentru proiect PCD: server UNIX, client ordinar si client de administrare pentru generare SQL dintr-o diagrama ER si validarea preventiva a batch-urilor `INSERT`.

## Build

```sh
make
```

Dependinte de sistem: compilator C, pthread si ncurses (`libncurses-dev` pe Debian/Ubuntu).

## Rulare

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

## Protocol

Protocolul pastreaza ideea din skeleton: fiecare mesaj are header fix cu `msg_size`, `client_id`, `op_id`, `flags`, urmat de payload binar. Transferul de fisiere este chunked prin `OP_UPLOAD_BEGIN`, `OP_UPLOAD_CHUNK`, `OP_UPLOAD_END`, cu bucati de 64 KB.

## Observatii

Parserul ER accepta formatul JSON din `examples/er_schema.json`. Parserul SQL acopera `INSERT INTO table [(cols...)] VALUES (...), (...);`, suficiente pentru validarea de chei primare, `UNIQUE`, `NOT NULL` si foreign key in demo.

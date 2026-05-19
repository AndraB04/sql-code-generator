# Software Requirements Specification - Milestone 2

## Scop

Aplicatia SQL Code Generator primeste o schema ER in format JSON, genereaza cod SQL `CREATE TABLE` pentru toate tabelele si relatiile dintre acestea si valideaza batch-uri `INSERT` inainte de aplicarea lor intr-o baza de date reala. Pentru Milestone 2, sistemul include server functional, client ordinar C, client ordinar alternativ Python si client de administrare ncurses.

## Actori

- ADMIN (UX): ruleaza pe aceeasi masina cu serverul si executa operatii de administrare fara transfer de fisiere.
- REMOTE/IN: client ordinar conectat prin socket INET, local sau de pe alta masina.
- Server: proceseaza clientii ordinari printr-o coada FIFO si adminul pe canal separat sincron.

## Cerinte Functionale

### Client ADMIN

1. Autentificare admin cu un singur administrator conectat simultan.
2. Deconectare automata la timeout de inactivitate configurabil.
3. Rapoarte pentru clienti conectati.
4. Rapoarte pentru comenzi totale, comenzi INSERT, comenzi esuate si comenzi anulate.
5. Rapoarte pentru durata medie de executie.
6. Rapoarte pentru istoric recent de operatii.
7. Rapoarte pentru tabele incarcate si numar de randuri.
8. Rapoarte pentru adancimea cozii de procesare.
9. Deconectarea fortata a unui client ordinar dupa `client_id`.
10. Anularea unei comenzi curente/pending dupa `client_id`.
11. Blocarea accesului dinspre un IP/domeniu.

### Client REMOTE/IN

1. Conectare TCP si primire `client_id` unic.
2. Transfer fisier schema ER client -> server prin bucati de 64 KB.
3. Incarcare schema ER JSON pe server.
4. Generare SQL `CREATE TABLE`.
5. Transfer fisier SQL generat server -> client prin bucati de 64 KB.
6. Validare sintactica batch `INSERT` cu `libpg_query`.
7. Validare semantica batch `INSERT` pentru anticiparea erorilor `constraint failed`.
8. Aplicare in memorie a inserturilor valide pentru validari ulterioare.
9. Tratare erori pentru tabela/coloana inexistenta, `NOT NULL`, `PRIMARY KEY`, `UNIQUE`, foreign key lipsa.
10. Verificare sincrona stare procesare prin `status [job_id|last]`.
11. Acces sincron la rezultatul procesarii prin `result [job_id|last]`.
12. Inchidere controlata prin `BYE`.
13. Client ordinar alternativ in Python pe acelasi protocol.

## Cerinte Nefunctionale

- Serverul accepta mai multi clienti ordinari simultan.
- Cererile clientilor ordinari sunt puse intr-o coada FIFO comuna.
- Serverul are fir de executie separat pentru interfata clientilor ordinari IN.
- Serverul are fir de executie separat pentru interfata admin UX.
- Canalul admin este sincron si separat de canalul IN.
- Buildul C trebuie sa treaca fara warning-uri cu `-Wall -Wextra -Wpedantic -Werror`.
- Codul trebuie sa fie compilat cu `-std=c11 -D_POSIX_C_SOURCE=200809L`.
- Repository-ul trebuie sa includa fisier `.clang-tidy` cu checks pentru analyzer, bugprone, cert, concurrency, misc, performance, portability si readability, tratate ca erori.
- Pentru profilare memorie trebuie sa existe tinte pentru ASan/LSan/UBSan si Valgrind Memcheck.
- Pentru concurenta trebuie sa existe tinte pentru TSan si Valgrind Helgrind.
- Serverul trebuie sa foloseasca `cJSON` pentru parsarea diagramei ER.
- Serverul trebuie sa foloseasca `libpg_query` pentru analiza sintactica a query-urilor SQL.
- Serverul nu trebuie sa execute scripturi Python/Java pentru procesarea datelor; prelucrarea are loc in C.
- Componenta web services, daca este adaugata ulterior, trebuie proiectata pe gSOAP/SOAP.
- Transferul de fisiere trebuie sa functioneze pentru fisiere modeste, pana la cateva sute de MB, prin chunking.
- Protocolul foloseste header fix binar: `msg_size`, `client_id`, `op_id`, `flags`.
- Configurarea porturilor si timeoutului admin se face prin `config.cfg`, variabile de mediu sau argumente CLI.
- Serverul trebuie sa scrie evenimentele demonstrabile intr-un fisier de logging.

## Mapare Milestone 2

- Admin finalizat: rapoarte 1-6 plus operatii active de nivel B.
- IN peste pragul de 70%: conectare, upload, generare, download, validare, aplicare, erori, inchidere, client alternativ.
- Transfer bidirectional: schema client -> server si SQL generat server -> client.
- Coada de procesare: `RequestQueue` cu mutex si condition variable.
- Job tracking: `QUEUED`, `RUNNING`, `DONE`, `ERROR`, cu acces la status si rezultat.
- Logging: `logs/server.log`.
- Procesare INSERT: cate un proces copil pentru fiecare batch, cu starea tabelelor in memorie partajata `mmap`.
- Biblioteci externe de procesare: `cJSON` si `libpg_query`.
- SRS imbunatatit fata de Milestone 1: include operatiile admin active, clientul Python si transferul bidirectional.

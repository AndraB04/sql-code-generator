# Software Requirements Specification

## 1. Scop

Aplicatia SQL Code Generator primeste o descriere ER in format JSON, genereaza cod SQL `CREATE TABLE` pentru toate tabelele si relatiile si valideaza batch-uri `INSERT` inainte de executie, anticipand erori de tip `constraint failed`.

## 2. Actori

- Client ordinar: trimite fisiere ER, cere generare SQL si trimite inserturi.
- Administrator: consulta rapoarte de stare printr-un client C cu ncurses.
- Server: proceseaza toate cererile ordinare intr-o singura coada FIFO.

## 3. Cerinte functionale

- Serverul asculta conexiuni TCP pentru clienti ordinari pe portul `18081`.
- Serverul asculta un port separat pentru admin pe `18082`.
- Un singur admin poate fi conectat simultan.
- Conexiunea admin expira dupa `60` secunde de inactivitate.
- Clientii ordinari primesc un `client_id` printr-o operatie de conectare.
- Serverul accepta transfer de fisiere mari prin mesaje chunked.
- Serverul incarca diagrama ER si construieste starea interna a tabelelor.
- Serverul genereaza instructiuni `CREATE TABLE` cu `PRIMARY KEY`, `UNIQUE`, `NOT NULL` si `REFERENCES`.
- Serverul valideaza batch-uri `INSERT` pentru tabele existente, coloane existente, valori `NULL`, duplicate si foreign key lipsa.
- Pentru fiecare cerere de insert/batch insert, serverul creeaza un proces copil care valideaza pe starea partajata.
- Dupa validare reusita, serverul aplica insertul in starea partajata.
- Adminul poate cere cel putin 6 rapoarte: clienti conectati, comenzi, durata medie, istoric, tabele, coada.

## 4. Cerinte nefunctionale

- Implementare server in C pe UNIX/Linux.
- Implementare client ordinar in C pe UNIX/Linux.
- Implementare client admin in C cu ncurses.
- Se foloseste `poll` pentru multiplexarea conexiunilor.
- Se folosesc thread-uri pentru worker-ul cozii ordinare si procese copil pentru validarea inserturilor.
- Protocolul este sincron la nivel de cerere-raspuns.
- Nu se folosesc mecanisme de proxy catre alte limbaje.

## 5. Limitari asumate

- Formatul ER acceptat este JSON-ul documentat in `examples/er_schema.json`.
- Parserul SQL este orientat pe `INSERT INTO ... VALUES ...`, nu pe intreg dialectul PostgreSQL.
- Stocarea demonstrativa din shared memory foloseste limite fixe: 32 tabele, 32 coloane/tabel si 1024 randuri/tabel.

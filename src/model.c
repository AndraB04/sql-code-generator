/*
 * Nume si prenume: Afrem Jasmine-Emilia si Bimbirica Andra
 * IR3 2026, grupa 2
 * model.c
 * In acest fisier implementam functiile principale pentru gestionarea
 * modelului de date al aplicatiei. Programul poate incarca o diagrama ER
 * dintr-un fisier JSON, poate genera instructiuni SQL CREATE TABLE,
 * poate parsa si valida instructiuni INSERT si poate produce rapoarte
 * administrative despre starea serverului si datele memorate.
 */

#include "model.h"      /* structuri si constante pentru modelul de date */

#include <cjson/cJSON.h> /* parser JSON extern cerut de tema */
#include <pg_query.h>   /* parser SQL PostgreSQL extern cerut de tema */

#include <ctype.h>      /* functii pentru testarea/manipularea caracterelor */
#include <errno.h>      /* errno pentru erori de fisiere */
#include <stdarg.h>     /* suport pentru functii cu numar variabil de argumente */
#include <stdio.h>      /* functii standard de intrare/iesire */
#include <stdlib.h>     /* alocare dinamica de memorie, conversii */
#include <string.h>     /* functii pentru siruri si memorie */
#include <time.h>       /* functii pentru timp calendaristic */

#define MAX_BATCH_ROWS 256       /* numarul maxim de randuri acceptate intr-un batch INSERT */
#define MAX_ER_FILE_SIZE (1024L * 1024L)

/* 
 * aceasta structura memoreaza un rand care trebuie inserat si tabelul
 * in care va fi adaugat acel rand
 */
typedef struct {
    int table_index;    /* indexul tabelului in vectorul de tabele */
    Row row;            /* valorile efective ale randului */
} InsertRow;

/*
 * aceasta structura grupeaza mai multe randuri parse-ate din una sau mai multe
 * instructiuni INSERT, pentru a fi validate si aplicate impreuna
 */
typedef struct {
    int count;                      /* cate randuri au fost adaugate in batch */
    InsertRow rows[MAX_BATCH_ROWS]; /* vectorul de randuri */
} InsertBatch;

/*
 * functie auxiliara folosita pentru a scrie un mesaj de eroare in bufferul err
 * daca bufferul are lungime valida
 */
static void set_err(char *err, size_t err_len, const char *msg) {
    if (err_len > 0) {
        snprintf(err, err_len, "%s", msg);
    }
}

/*
 * functie auxiliara care adauga text formatat la finalul sirului out
 * se foloseste pentru construirea treptata a unor raspunsuri lungi
 * precum SQL generat sau rapoarte administrative
 */
static void appendf(char *out, size_t out_len, const char *fmt, ...) {
    size_t used = strlen(out);   /* calculam cati octeti sunt deja ocupati */

    /* daca bufferul este deja plin, nu mai adaugam nimic */
    if (used >= out_len) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, out_len - used, fmt, ap);
    va_end(ap);
}

/*
 * elimina spatiile albe de la inceputul si de la sfarsitul sirului
 * modificarile se fac direct in acelasi buffer
 */
static void trim_inplace(char *s) {
    size_t len = strlen(s);

    /* eliminam spatiile de la finalul sirului */
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }

    /* identificam prima pozitie utila, diferita de spatiu */
    size_t start = 0;
    while (s[start] != '\0' && isspace((unsigned char)s[start])) {
        start++;
    }

    /* daca exista spatii la inceput, mutam continutul spre stanga */
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

/*
 * verifica daca sirul s incepe cu prefix, fara a tine cont de litere
 * mari sau mici; este util pentru a accepta INSERT, insert, Insert etc.
 */
static int ci_starts(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

/*
 * cauta un subsir needle in sirul haystack fara a tine cont de litere
 * mari sau mici; returneaza adresa de inceput daca a fost gasit
 */
static const char *ci_find(const char *haystack, const char *needle) {
    size_t n = strlen(needle);

    if (n == 0) {
        return haystack;
    }

    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;

        while (i < n &&
               p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }

        if (i == n) {
            return p;
        }
    }

    return NULL;
}

/*
 * sare peste toate caracterele de tip whitespace si returneaza pozitia
 * primului caracter relevant din sir
 */
static const char *skip_ws(const char *p) {
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

/*
 * parseaza un identificator SQL: nume de tabela sau de coloana
 * accepta atat forma simpla (ex: users), cat si forma intre ghilimele
 * sau backticks
 */
static int parse_identifier(const char **pp, char *out, size_t out_len) {
    const char *p = skip_ws(*pp);
    size_t n = 0;

    /* identificator intre ghilimele duble sau backticks */
    if (*p == '"' || *p == '`') {
        char quote = *p++;
        while (*p != '\0' && *p != quote && n + 1 < out_len) {
            out[n++] = *p++;
        }

        /* daca nu gasim delimitatorul de inchidere, parsarea esueaza */
        if (*p != quote) {
            return 0;
        }
        p++;
    } else {
        /* identificator obisnuit: litere, cifre sau underscore */
        while (*p != '\0' &&
               (isalnum((unsigned char)*p) || *p == '_') &&
               n + 1 < out_len) {
            out[n++] = *p++;
        }
    }

    out[n] = '\0';

    if (n == 0) {
        return 0;
    }

    *pp = p;   /* actualizam pozitia curenta in sir */
    return 1;
}

/*
 * cauta o tabela dupa nume in starea comuna a aplicatiei
 * returneaza indexul tabelului daca exista, altfel -1
 */
static int find_table(const SharedState *state, const char *name) {
    for (int i = 0; i < state->table_count; i++) {
        if (strcmp(state->tables[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * cauta o coloana dupa nume in cadrul unei tabele
 * returneaza indexul coloanei sau -1 daca nu exista
 */
static int find_column(const Table *table, const char *name) {
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * separa un nume calificat de forma tabela.coloana in doua siruri:
 * partea din stanga si partea din dreapta punctului
 */
static void split_qualified(const char *s, char *left, size_t left_len, char *right, size_t right_len) {
    const char *dot = strchr(s, '.');

    if (dot == NULL) {
        snprintf(left, left_len, "%s", s);
        right[0] = '\0';
        return;
    }

    snprintf(left, left_len, "%.*s", (int)(dot - s), s);
    snprintf(right, right_len, "%s", dot + 1);
}

/*
 * initializeaza structura SharedState cu valori implicite:
 * - toate campurile pe 0
 * - timpul pornirii serverului
 * - id-ul urmatorului client setat la 1
 */
void state_init(SharedState *state) {
    memset(state, 0, sizeof(*state));
    state->started_at = time(NULL);
    state->next_client_id = 1;
    state->next_job_id = 1;
}

/*
 * citeste complet un fisier JSON in memorie pentru a fi pasat catre cJSON
 */
static char *read_json_file(const char *path, size_t *json_len, char *err, size_t err_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_err(err, err_len, "cannot open ER file");
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, err_len, "cannot seek ER file");
        return NULL;
    }

    errno = 0;
    long len = ftell(f);
    if (len < 0) {
        int saved_errno = errno;
        fclose(f);
        snprintf(err, err_len, "cannot tell ER file size: %s", strerror(saved_errno));
        return NULL;
    }

    if (len == 0 || len > MAX_ER_FILE_SIZE) {
        fclose(f);
        set_err(err, err_len, "ER file is empty or too large");
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_err(err, err_len, "cannot rewind ER file");
        return NULL;
    }

    char *json = (char *)malloc((size_t)len + 1);
    if (json == NULL) {
        fclose(f);
        set_err(err, err_len, "out of memory");
        return NULL;
    }

    if (fread(json, 1, (size_t)len, f) != (size_t)len) {
        free(json);
        fclose(f);
        set_err(err, err_len, "cannot read ER file");
        return NULL;
    }

    fclose(f);
    json[len] = '\0';
    *json_len = (size_t)len;
    return json;
}

/*
 * copiaza o valoare string cJSON intr-un buffer fix
 */
static int cjson_copy_string(const cJSON *object, const char *key, char *out, size_t out_len) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return 0;
    }
    snprintf(out, out_len, "%s", item->valuestring);
    return 1;
}

/*
 * citeste un boolean din cJSON, acceptand si forma numerica 0/1
 */
static int cjson_get_bool(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return 0;
}

/*
 * completeaza o coloana pe baza obiectului cJSON primit
 */
static int load_column_json(Column *column, const cJSON *column_json, char *err, size_t err_len) {
    memset(column, 0, sizeof(*column));
    if (!cjson_copy_string(column_json, "name", column->name, sizeof(column->name))) {
        set_err(err, err_len, "column without name");
        return -1;
    }
    if (!cjson_copy_string(column_json, "type", column->type, sizeof(column->type))) {
        snprintf(column->type, sizeof(column->type), "TEXT");
    }

    column->primary_key = cjson_get_bool(column_json, "primary_key");
    column->unique = cjson_get_bool(column_json, "unique");
    column->not_null = cjson_get_bool(column_json, "not_null") || column->primary_key;

    char ref[MAX_NAME * 2];
    if (cjson_copy_string(column_json, "references", ref, sizeof(ref))) {
        split_qualified(ref,
                        column->ref_table, sizeof(column->ref_table),
                        column->ref_column, sizeof(column->ref_column));
    }
    return 0;
}

/*
 * incarca descrierea diagramei ER dintr-un fisier JSON
 * si completeaza tabelele, coloanele si relatiile in structura state
 */
int load_er_json(SharedState *state, const char *path, char *err, size_t err_len) {
    size_t json_len = 0;
    char *json = read_json_file(path, &json_len, err, err_len);
    if (json == NULL) {
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL) {
        free(json);
        set_err(err, err_len, "invalid ER JSON");
        return -1;
    }

    const cJSON *tables = cJSON_GetObjectItemCaseSensitive(root, "tables");
    if (!cJSON_IsArray(tables)) {
        cJSON_Delete(root);
        free(json);
        set_err(err, err_len, "missing tables array");
        return -1;
    }

    state->table_count = 0;

    const cJSON *table_json = NULL;
    cJSON_ArrayForEach(table_json, tables) {
        if (state->table_count >= MAX_TABLES) {
            break;
        }
        if (!cJSON_IsObject(table_json)) {
            cJSON_Delete(root);
            free(json);
            set_err(err, err_len, "table entry must be object");
            return -1;
        }
        Table *t = &state->tables[state->table_count];
        memset(t, 0, sizeof(*t));

        if (!cjson_copy_string(table_json, "name", t->name, sizeof(t->name))) {
            cJSON_Delete(root);
            free(json);
            set_err(err, err_len, "table without name");
            return -1;
        }

        const cJSON *columns = cJSON_GetObjectItemCaseSensitive(table_json, "columns");
        if (!cJSON_IsArray(columns)) {
            cJSON_Delete(root);
            free(json);
            set_err(err, err_len, "table without columns");
            return -1;
        }

        const cJSON *column_json = NULL;
        cJSON_ArrayForEach(column_json, columns) {
            if (t->column_count >= MAX_COLUMNS) {
                break;
            }
            if (!cJSON_IsObject(column_json)) {
                cJSON_Delete(root);
                free(json);
                set_err(err, err_len, "column entry must be object");
                return -1;
            }
            Column *c = &t->columns[t->column_count];
            if (load_column_json(c, column_json, err, err_len) < 0) {
                cJSON_Delete(root);
                free(json);
                return -1;
            }
            t->column_count++;
        }

        if (t->column_count == 0) {
            cJSON_Delete(root);
            free(json);
            set_err(err, err_len, "table has no columns");
            return -1;
        }

        state->table_count++;
    }

    /* optional, procesam si relatiile separate din array-ul "relations" */
    const cJSON *relations = cJSON_GetObjectItemCaseSensitive(root, "relations");
    if (cJSON_IsArray(relations)) {
        const cJSON *relation_json = NULL;
        cJSON_ArrayForEach(relation_json, relations) {
            char from[MAX_NAME * 2];
            char to[MAX_NAME * 2];
            char from_table[MAX_NAME];
            char from_col[MAX_NAME];
            char to_table[MAX_NAME];
            char to_col[MAX_NAME];

            if (!cJSON_IsObject(relation_json) ||
                !cjson_copy_string(relation_json, "from", from, sizeof(from)) ||
                !cjson_copy_string(relation_json, "to", to, sizeof(to))) {
                continue;
            }

            split_qualified(from, from_table, sizeof(from_table), from_col, sizeof(from_col));
            split_qualified(to, to_table, sizeof(to_table), to_col, sizeof(to_col));

            int ti = find_table(state, from_table);
            if (ti >= 0) {
                int ci = find_column(&state->tables[ti], from_col);
                if (ci >= 0) {
                    snprintf(state->tables[ti].columns[ci].ref_table, MAX_NAME, "%s", to_table);
                    snprintf(state->tables[ti].columns[ci].ref_column, MAX_NAME, "%s", to_col);
                }
            }
        }
    }

    cJSON_Delete(root);
    free(json);
    snprintf(err, err_len, "loaded %d tables", state->table_count);
    return 0;
}

/*
 * genereaza instructiunile SQL CREATE TABLE pe baza modelului ER incarcat
 * rezultatul este scris in bufferul out
 */
int generate_sql(const SharedState *state, char *out, size_t out_len) {
    out[0] = '\0';

    if (state->table_count == 0) {
        snprintf(out, out_len, "-- No ER diagram loaded.\n");
        return 0;
    }

    for (int i = 0; i < state->table_count; i++) {
        const Table *t = &state->tables[i];

        appendf(out, out_len, "CREATE TABLE %s (\n", t->name);

        for (int j = 0; j < t->column_count; j++) {
            const Column *c = &t->columns[j];

            /* adaugam numele si tipul coloanei */
            appendf(out, out_len, "    %s %s", c->name, c->type);

            /* adaugam constrangerile in functie de proprietati */
            if (c->primary_key) {
                appendf(out, out_len, " PRIMARY KEY");
            }
            if (c->not_null && !c->primary_key) {
                appendf(out, out_len, " NOT NULL");
            }
            if (c->unique && !c->primary_key) {
                appendf(out, out_len, " UNIQUE");
            }
            if (c->ref_table[0] != '\0') {
                appendf(out, out_len, " REFERENCES %s(%s)", c->ref_table, c->ref_column);
            }

            /* punem virgula intre coloane, mai putin dupa ultima */
            appendf(out, out_len, "%s\n", j + 1 == t->column_count ? "" : ",");
        }

        appendf(out, out_len, ");\n\n");
    }

    return 0;
}

/*
 * parseaza o valoare individuala din lista VALUES a unei instructiuni INSERT
 * functia detecteaza si cazul special NULL
 */
static int parse_value(const char **pp, char *value, size_t value_len, int *is_null) {
    const char *p = skip_ws(*pp);
    size_t n = 0;
    *is_null = 0;

    /* valoare string delimitata de apostrof */
    if (*p == '\'') {
        p++;
        while (*p != '\0') {
            /* doua apostrofuri consecutive inseamna apostrof in text */
            if (*p == '\'' && p[1] == '\'') {
                if (n + 1 < value_len) {
                    value[n++] = '\'';
                }
                p += 2;
                continue;
            }

            if (*p == '\'') {
                p++;
                break;
            }

            if (n + 1 < value_len) {
                value[n++] = *p;
            }
            p++;
        }
    } else {
        /* valoare simpla pana la virgula sau paranteza inchisa */
        while (*p != '\0' && *p != ',' && *p != ')') {
            if (n + 1 < value_len) {
                value[n++] = *p;
            }
            p++;
        }
    }

    value[n] = '\0';
    trim_inplace(value);

    /* daca textul este exact NULL, il marcam separat */
    if (strcmp(value, "NULL") == 0) {
        *is_null = 1;
        value[0] = '\0';
    }

    *pp = p;
    return 1;
}
/*
 * parseaza o instructiune INSERT completa si o transforma intr-un batch intern
 * functia verifica sintaxa de baza: INSERT INTO tabela (...) VALUES (...)
 */
static int parse_insert_statement(const SharedState *state, const char *stmt, InsertBatch *batch,
                                  char *err, size_t err_len) {
    const char *p = skip_ws(stmt);

    /* ignoram instructiunile goale */
    if (*p == '\0') {
        return 0;
    }

    /* acceptam doar instructiuni de tip INSERT */
    if (!ci_starts(p, "insert")) {
        set_err(err, err_len, "only INSERT statements are accepted");
        return -1;
    }
    p += 6;

    p = skip_ws(p);

    /* dupa INSERT trebuie sa urmeze INTO */
    if (!ci_starts(p, "into")) {
        set_err(err, err_len, "expected INTO");
        return -1;
    }
    p += 4;

    /* extragem numele tabelei in care se insereaza */
    char table_name[MAX_NAME];
    if (!parse_identifier(&p, table_name, sizeof(table_name))) {
        set_err(err, err_len, "expected table name");
        return -1;
    }

    int table_index = find_table(state, table_name);
    if (table_index < 0) {
        snprintf(err, err_len, "constraint failed: table '%s' does not exist", table_name);
        return -1;
    }

    const Table *table = &state->tables[table_index];

    /*
     * column_map retine pentru fiecare valoare din VALUES
     * coloana corespunzatoare din tabela
     */
    int column_map[MAX_COLUMNS];
    int value_columns = 0;

    p = skip_ws(p);

    /*
     * daca exista o lista explicita de coloane:
     * INSERT INTO users (id, name) VALUES (...)
     */
    if (*p == '(') {
        p++;

        while (*p != '\0' && *p != ')') {
            char col_name[MAX_NAME];

            if (!parse_identifier(&p, col_name, sizeof(col_name))) {
                set_err(err, err_len, "expected column name");
                return -1;
            }

            int ci = find_column(table, col_name);
            if (ci < 0) {
                snprintf(err, err_len, "constraint failed: unknown column '%s.%s'",
                         table_name, col_name);
                return -1;
            }

            column_map[value_columns++] = ci;
            p = skip_ws(p);

            if (*p == ',') {
                p++;
            } else if (*p != ')') {
                set_err(err, err_len, "expected comma or ')' in column list");
                return -1;
            }
        }

        if (*p != ')') {
            set_err(err, err_len, "unterminated column list");
            return -1;
        }
        p++;
    } else {
        /*
         * daca lista de coloane nu este specificata,
         * presupunem toate coloanele in ordinea lor naturala
         */
        value_columns = table->column_count;
        for (int i = 0; i < value_columns; i++) {
            column_map[i] = i;
        }
    }

    /* cautam cuvantul cheie VALUES */
    const char *values = ci_find(p, "values");
    if (values == NULL) {
        set_err(err, err_len, "expected VALUES");
        return -1;
    }
    p = values + 6;

    /*
     * parcurgem toate tuplele de valori:
     * VALUES (...), (...), (...)
     */
    for (;;) {
        p = skip_ws(p);

        if (*p == '\0') {
            break;
        }

        if (*p == ',') {
            p++;
            continue;
        }

        if (*p != '(') {
            set_err(err, err_len, "expected '(' before values tuple");
            return -1;
        }
        p++;

        if (batch->count >= MAX_BATCH_ROWS) {
            set_err(err, err_len, "too many rows in insert batch");
            return -1;
        }

        InsertRow *ir = &batch->rows[batch->count];
        memset(ir, 0, sizeof(*ir));
        ir->table_index = table_index;

        /* initial marcam toate coloanele ca NULL */
        for (int i = 0; i < table->column_count; i++) {
            ir->row.is_null[i] = 1;
        }

        int value_index = 0;

        while (*p != '\0' && *p != ')') {
            char value[MAX_VALUE];
            int is_null = 0;

            if (value_index >= value_columns) {
                set_err(err, err_len, "too many values");
                return -1;
            }

            parse_value(&p, value, sizeof(value), &is_null);

            /* mapam valoarea pe coloana corecta */
            int ci = column_map[value_index++];
            snprintf(ir->row.values[ci], MAX_VALUE, "%s", value);
            ir->row.is_null[ci] = is_null;

            p = skip_ws(p);

            if (*p == ',') {
                p++;
            } else if (*p != ')') {
                set_err(err, err_len, "expected comma or ')' in values tuple");
                return -1;
            }
        }

        if (*p != ')') {
            set_err(err, err_len, "unterminated values tuple");
            return -1;
        }

        /* verificam sa existe acelasi numar de coloane si valori */
        if (value_index != value_columns) {
            set_err(err, err_len, "column count and value count differ");
            return -1;
        }

        p++;
        batch->count++;
    }

    return 0;
}

/*
 * parseaza un text SQL ce poate contine mai multe instructiuni INSERT
 * separate prin caracterul ';'
 */
static int parse_batch(const SharedState *state, const char *sql, InsertBatch *batch,
                       char *err, size_t err_len) {
    memset(batch, 0, sizeof(*batch));

    const char *start = sql;
    int in_string = 0;

    /*
     * impartim textul in instructiuni individuale
     * dar ignoram ';' aparut in interiorul stringurilor SQL
     */
    for (const char *p = sql;; p++) {
        if (*p == '\'' && (p == sql || p[-1] != '\\')) {
            in_string = !in_string;
        }

        if ((*p == ';' && !in_string) || *p == '\0') {
            char stmt[16384];
            size_t len = (size_t)(p - start);

            if (len >= sizeof(stmt)) {
                set_err(err, err_len, "statement too large");
                return -1;
            }

            memcpy(stmt, start, len);
            stmt[len] = '\0';
            trim_inplace(stmt);

            /* parseam fiecare instructiune nenula */
            if (stmt[0] != '\0' &&
                parse_insert_statement(state, stmt, batch, err, err_len) < 0) {
                return -1;
            }

            if (*p == '\0') {
                break;
            }

            start = p + 1;
        }
    }

    if (batch->count == 0) {
        set_err(err, err_len, "empty insert batch");
        return -1;
    }

    return 0;
}

/*
 * verifica daca o valoare exista deja pe o anumita coloana,
 * fie in datele deja stocate, fie in randurile anterioare din batch
 */
static int value_exists(const SharedState *state, const InsertBatch *batch, int upto,
                        int table_index, int col_index, const char *value) {
    const Table *table = &state->tables[table_index];

    /* verificam mai intai randurile deja existente in tabela */
    for (int r = 0; r < table->row_count; r++) {
        if (!table->rows[r].is_null[col_index] &&
            strcmp(table->rows[r].values[col_index], value) == 0) {
            return 1;
        }
    }

    /* apoi verificam randurile din batch deja parcurse */
    for (int i = 0; i < upto; i++) {
        if (batch->rows[i].table_index == table_index &&
            !batch->rows[i].row.is_null[col_index] &&
            strcmp(batch->rows[i].row.values[col_index], value) == 0) {
            return 1;
        }
    }

    return 0;
}

/* functie auxiliara care doar redenumeste semantic verificarea de duplicat */
static int duplicate_value(const SharedState *state, const InsertBatch *batch, int upto,
                           int table_index, int col_index, const char *value) {
    return value_exists(state, batch, upto, table_index, col_index, value);
}

/*
 * valideaza toate randurile din batch conform regulilor din model:
 * - NOT NULL / PRIMARY KEY
 * - UNIQUE / PRIMARY KEY fara duplicate
 * - FOREIGN KEY sa indice catre valori existente
 */
static int validate_batch_rows(const SharedState *state, const InsertBatch *batch,
                               char *err, size_t err_len) {
    for (int i = 0; i < batch->count; i++) {
        const InsertRow *ir = &batch->rows[i];
        const Table *table = &state->tables[ir->table_index];

        /* verificam limita de stocare interna demonstrativa */
        if (table->row_count >= MAX_ROWS) {
            snprintf(err, err_len, "constraint failed: table '%s' is full in demo storage",
                     table->name);
            return -1;
        }

        for (int c = 0; c < table->column_count; c++) {
            const Column *col = &table->columns[c];

            /* campurile NOT NULL sau PRIMARY KEY nu pot avea valoarea NULL */
            if ((col->not_null || col->primary_key) && ir->row.is_null[c]) {
                snprintf(err, err_len, "constraint failed: %s.%s cannot be NULL",
                         table->name, col->name);
                return -1;
            }

            /* campurile UNIQUE / PRIMARY KEY nu trebuie sa contina duplicate */
            if ((col->primary_key || col->unique) && !ir->row.is_null[c]) {
                if (duplicate_value(state, batch, i, ir->table_index, c, ir->row.values[c])) {
                    snprintf(err, err_len, "constraint failed: duplicate value for %s.%s",
                             table->name, col->name);
                    return -1;
                }
            }

            /*
             * daca exista referinta externa, valoarea trebuie sa existe
             * in tabela si coloana referita
             */
            if (col->ref_table[0] != '\0' && !ir->row.is_null[c]) {
                int rt = find_table(state, col->ref_table);
                if (rt < 0) {
                    snprintf(err, err_len, "constraint failed: referenced table '%s' missing",
                             col->ref_table);
                    return -1;
                }

                int rc = find_column(&state->tables[rt], col->ref_column);
                if (rc < 0) {
                    snprintf(err, err_len,
                             "constraint failed: referenced column '%s.%s' missing",
                             col->ref_table, col->ref_column);
                    return -1;
                }

                if (!value_exists(state, batch, i, rt, rc, ir->row.values[c])) {
                    snprintf(err, err_len,
                             "constraint failed: foreign key %s.%s references missing %s.%s=%s",
                             table->name, col->name,
                             col->ref_table, col->ref_column,
                             ir->row.values[c]);
                    return -1;
                }
            }
        }
    }

    return 0;
}

/*
 * foloseste libpg_query pentru validarea sintactica PostgreSQL a batch-ului SQL
 */
static int validate_sql_syntax_pg_query(const char *sql, char *err, size_t err_len) {
    PgQueryParseResult result = pg_query_parse(sql);
    if (result.error != NULL) {
        snprintf(err, err_len, "SQL syntax error: %s", result.error->message);
        pg_query_free_parse_result(result);
        return -1;
    }
    pg_query_free_parse_result(result);
    return 0;
}

/*
 * valideaza un batch de INSERT-uri fara a-l aplica efectiv
 * este folosita pentru a spune daca instructiunile sunt corecte
 */
int validate_insert_batch(const SharedState *state, const char *sql, char *err, size_t err_len) {
    if (state->table_count == 0) {
        set_err(err, err_len, "no ER diagram loaded");
        return -1;
    }

    InsertBatch batch;

    if (validate_sql_syntax_pg_query(sql, err, err_len) < 0) {
        return -1;
    }

    if (parse_batch(state, sql, &batch, err, err_len) < 0) {
        return -1;
    }

    if (validate_batch_rows(state, &batch, err, err_len) < 0) {
        return -1;
    }

    snprintf(err, err_len, "OK: %d row(s) accepted", batch.count);
    return 0;
}

/*
 * parseaza, valideaza si apoi aplica efectiv un batch de INSERT-uri
 * randurile validate sunt copiate in tabelele corespunzatoare
 */
int apply_insert_batch(SharedState *state, const char *sql, char *err, size_t err_len) {
    InsertBatch batch;

    if (validate_sql_syntax_pg_query(sql, err, err_len) < 0) {
        return -1;
    }

    if (parse_batch(state, sql, &batch, err, err_len) < 0) {
        return -1;
    }

    if (validate_batch_rows(state, &batch, err, err_len) < 0) {
        return -1;
    }

    /* adaugam fiecare rand in tabela lui */
    for (int i = 0; i < batch.count; i++) {
        Table *table = &state->tables[batch.rows[i].table_index];
        table->rows[table->row_count++] = batch.rows[i].row;
    }

    snprintf(err, err_len, "OK: inserted %d row(s)", batch.count);
    return 0;
}

/*
 * genereaza un raport administrativ in functie de categoria ceruta:
 * clients, commands, avg, history, tables sau queue
 */
void admin_report(const SharedState *state, const char *category, char *out, size_t out_len) {
    out[0] = '\0';

    if (strcmp(category, "clients") == 0) {
        appendf(out, out_len, "Connected ordinary clients: %d\n", state->client_count);

        /* afisam informatii despre clientii activi */
        for (int i = 0; i < 128; i++) {
            if (state->clients[i].fd > 0) {
                appendf(out, out_len, "client=%d fd=%d addr=%s idle=%lds\n",
                        state->clients[i].client_id,
                        state->clients[i].fd,
                        state->clients[i].addr,
                        (long)(time(NULL) - state->clients[i].last_seen));
            }
        }

    } else if (strcmp(category, "commands") == 0) {
        appendf(out, out_len,
                "Total commands: %ld\nInsert commands: %ld\nFailed commands: %ld\nCancelled commands: %ld\n",
                state->total_commands,
                state->insert_commands,
                state->failed_commands,
                state->cancelled_commands);
        if (state->active_client_id > 0) {
            appendf(out, out_len, "Active command: client=%d op=%d running=%lds\n",
                    state->active_client_id,
                    state->active_op_id,
                    (long)(time(NULL) - state->active_started_at));
        } else {
            appendf(out, out_len, "Active command: none\n");
        }

    } else if (strcmp(category, "avg") == 0) {
        long avg = state->total_commands ? state->total_exec_ms / state->total_commands : 0;
        appendf(out, out_len, "Average execution time: %ld ms\n", avg);

    } else if (strcmp(category, "history") == 0) {
        appendf(out, out_len, "Recent history:\n");

        /* luam doar ultimele MAX_HISTORY intrari */
        int start = state->history_count > MAX_HISTORY
                        ? state->history_count - MAX_HISTORY
                        : 0;

        for (int i = start; i < state->history_count; i++) {
            appendf(out, out_len, "%s\n", state->history[i % MAX_HISTORY]);
        }

    } else if (strcmp(category, "tables") == 0) {
        appendf(out, out_len, "Tables loaded: %d\n", state->table_count);

        /* pentru fiecare tabela afisam cate coloane si cate randuri are */
        for (int i = 0; i < state->table_count; i++) {
            appendf(out, out_len, "%s: %d columns, %d rows\n",
                    state->tables[i].name,
                    state->tables[i].column_count,
                    state->tables[i].row_count);
        }

    } else if (strcmp(category, "queue") == 0) {
        appendf(out, out_len, "Ordinary request queue depth: %d\n", state->queue_depth);
        if (state->cancel_client_id > 0) {
            appendf(out, out_len, "Pending cancellation for client: %d\n", state->cancel_client_id);
        }

    } else if (strcmp(category, "jobs") == 0) {
        appendf(out, out_len, "Recent processing jobs:\n");
        for (int i = 0; i < MAX_JOBS; i++) {
            if (state->jobs[i].job_id > 0) {
                appendf(out, out_len, "job=%d client=%d op=%d status=%s\n",
                        state->jobs[i].job_id,
                        state->jobs[i].client_id,
                        state->jobs[i].op_id,
                        state->jobs[i].status);
            }
        }

    } else {
        appendf(out, out_len,
                "Unknown category. Use: clients, commands, avg, history, tables, queue, jobs\n");
    }
}

#include "model.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_BATCH_ROWS 256

typedef struct {
    int table_index;
    Row row;
} InsertRow;

typedef struct {
    int count;
    InsertRow rows[MAX_BATCH_ROWS];
} InsertBatch;

static void set_err(char *err, size_t err_len, const char *msg) {
    if (err_len > 0) {
        snprintf(err, err_len, "%s", msg);
    }
}

static void appendf(char *out, size_t out_len, const char *fmt, ...) {
    size_t used = strlen(out);
    if (used >= out_len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + used, out_len - used, fmt, ap);
    va_end(ap);
}

static void trim_inplace(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (s[start] != '\0' && isspace((unsigned char)s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
}

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

static const char *ci_find(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) {
        return haystack;
    }
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < n && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

static const char *skip_ws(const char *p) {
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static int parse_identifier(const char **pp, char *out, size_t out_len) {
    const char *p = skip_ws(*pp);
    size_t n = 0;
    if (*p == '"' || *p == '`') {
        char quote = *p++;
        while (*p != '\0' && *p != quote && n + 1 < out_len) {
            out[n++] = *p++;
        }
        if (*p != quote) {
            return 0;
        }
        p++;
    } else {
        while (*p != '\0' && (isalnum((unsigned char)*p) || *p == '_') && n + 1 < out_len) {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    if (n == 0) {
        return 0;
    }
    *pp = p;
    return 1;
}

static int find_table(const SharedState *state, const char *name) {
    for (int i = 0; i < state->table_count; i++) {
        if (strcmp(state->tables[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_column(const Table *table, const char *name) {
    for (int i = 0; i < table->column_count; i++) {
        if (strcmp(table->columns[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *find_key(const char *json, const char *key) {
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_key(json, key);
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    p++;
    p = skip_ws(p);
    if (*p != '"') {
        return 0;
    }
    p++;
    size_t n = 0;
    while (*p != '\0' && *p != '"' && n + 1 < out_len) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    return *p == '"';
}

static int json_get_bool(const char *json, const char *key) {
    const char *p = find_key(json, key);
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    p = skip_ws(p + 1);
    return strncmp(p, "true", 4) == 0 || strncmp(p, "1", 1) == 0;
}

static int json_extract_array(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_key(json, key);
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, '[');
    if (p == NULL) {
        return 0;
    }

    int depth = 0;
    int in_string = 0;
    size_t n = 0;
    for (; *p != '\0'; p++) {
        char c = *p;
        if (n + 1 < out_len) {
            out[n++] = c;
        }
        if (c == '"' && (p == json || p[-1] != '\\')) {
            in_string = !in_string;
        }
        if (!in_string) {
            if (c == '[') {
                depth++;
            } else if (c == ']') {
                depth--;
                if (depth == 0) {
                    out[n] = '\0';
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int next_object(const char *array_json, int *pos, char *out, size_t out_len) {
    const char *p = array_json + *pos;
    while (*p != '\0' && *p != '{') {
        p++;
    }
    if (*p == '\0') {
        return 0;
    }

    int depth = 0;
    int in_string = 0;
    size_t n = 0;
    const char *start = p;
    for (; *p != '\0'; p++) {
        char c = *p;
        if (n + 1 < out_len) {
            out[n++] = c;
        }
        if (c == '"' && (p == start || p[-1] != '\\')) {
            in_string = !in_string;
        }
        if (!in_string) {
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    out[n] = '\0';
                    *pos = (int)(p - array_json + 1);
                    return 1;
                }
            }
        }
    }
    return 0;
}

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

void state_init(SharedState *state) {
    memset(state, 0, sizeof(*state));
    state->started_at = time(NULL);
    state->next_client_id = 1;
}

int load_er_json(SharedState *state, const char *path, char *err, size_t err_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        set_err(err, err_len, "cannot open ER file");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, err_len, "cannot seek ER file");
        return -1;
    }
    long len = ftell(f);
    if (len <= 0 || len > 1024 * 1024) {
        fclose(f);
        set_err(err, err_len, "ER file is empty or too large");
        return -1;
    }
    rewind(f);
    char *json = (char *)malloc((size_t)len + 1);
    if (json == NULL) {
        fclose(f);
        set_err(err, err_len, "out of memory");
        return -1;
    }
    if (fread(json, 1, (size_t)len, f) != (size_t)len) {
        free(json);
        fclose(f);
        set_err(err, err_len, "cannot read ER file");
        return -1;
    }
    fclose(f);
    json[len] = '\0';

    char tables_json[1024 * 1024];
    if (!json_extract_array(json, "tables", tables_json, sizeof(tables_json))) {
        free(json);
        set_err(err, err_len, "missing tables array");
        return -1;
    }

    state->table_count = 0;
    for (int pos = 0; state->table_count < MAX_TABLES;) {
        char table_obj[32768];
        if (!next_object(tables_json, &pos, table_obj, sizeof(table_obj))) {
            break;
        }
        Table *t = &state->tables[state->table_count];
        memset(t, 0, sizeof(*t));
        if (!json_get_string(table_obj, "name", t->name, sizeof(t->name))) {
            free(json);
            set_err(err, err_len, "table without name");
            return -1;
        }
        char columns_json[32768];
        if (!json_extract_array(table_obj, "columns", columns_json, sizeof(columns_json))) {
            free(json);
            set_err(err, err_len, "table without columns");
            return -1;
        }
        for (int cpos = 0; t->column_count < MAX_COLUMNS;) {
            char col_obj[4096];
            if (!next_object(columns_json, &cpos, col_obj, sizeof(col_obj))) {
                break;
            }
            Column *c = &t->columns[t->column_count];
            memset(c, 0, sizeof(*c));
            if (!json_get_string(col_obj, "name", c->name, sizeof(c->name))) {
                free(json);
                set_err(err, err_len, "column without name");
                return -1;
            }
            if (!json_get_string(col_obj, "type", c->type, sizeof(c->type))) {
                snprintf(c->type, sizeof(c->type), "TEXT");
            }
            c->primary_key = json_get_bool(col_obj, "primary_key");
            c->unique = json_get_bool(col_obj, "unique");
            c->not_null = json_get_bool(col_obj, "not_null") || c->primary_key;
            char ref[MAX_NAME * 2];
            if (json_get_string(col_obj, "references", ref, sizeof(ref))) {
                split_qualified(ref, c->ref_table, sizeof(c->ref_table), c->ref_column, sizeof(c->ref_column));
            }
            t->column_count++;
        }
        if (t->column_count == 0) {
            free(json);
            set_err(err, err_len, "table has no columns");
            return -1;
        }
        state->table_count++;
    }

    char rels_json[32768];
    if (json_extract_array(json, "relations", rels_json, sizeof(rels_json))) {
        for (int pos = 0;;) {
            char rel_obj[4096], from[MAX_NAME * 2], to[MAX_NAME * 2];
            char from_table[MAX_NAME], from_col[MAX_NAME], to_table[MAX_NAME], to_col[MAX_NAME];
            if (!next_object(rels_json, &pos, rel_obj, sizeof(rel_obj))) {
                break;
            }
            if (!json_get_string(rel_obj, "from", from, sizeof(from)) ||
                !json_get_string(rel_obj, "to", to, sizeof(to))) {
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

    free(json);
    snprintf(err, err_len, "loaded %d tables", state->table_count);
    return 0;
}

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
            appendf(out, out_len, "    %s %s", c->name, c->type);
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
            appendf(out, out_len, "%s\n", j + 1 == t->column_count ? "" : ",");
        }
        appendf(out, out_len, ");\n\n");
    }
    return 0;
}

static int parse_value(const char **pp, char *value, size_t value_len, int *is_null) {
    const char *p = skip_ws(*pp);
    size_t n = 0;
    *is_null = 0;
    if (*p == '\'') {
        p++;
        while (*p != '\0') {
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
        while (*p != '\0' && *p != ',' && *p != ')') {
            if (n + 1 < value_len) {
                value[n++] = *p;
            }
            p++;
        }
    }
    value[n] = '\0';
    trim_inplace(value);
    if (ci_starts(value, "NULL") && value[4] == '\0') {
        *is_null = 1;
        value[0] = '\0';
    }
    *pp = p;
    return 1;
}

static int parse_insert_statement(const SharedState *state, const char *stmt, InsertBatch *batch,
                                  char *err, size_t err_len) {
    const char *p = skip_ws(stmt);
    if (*p == '\0') {
        return 0;
    }
    if (!ci_starts(p, "insert")) {
        set_err(err, err_len, "only INSERT statements are accepted");
        return -1;
    }
    p += 6;
    p = skip_ws(p);
    if (!ci_starts(p, "into")) {
        set_err(err, err_len, "expected INTO");
        return -1;
    }
    p += 4;

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

    int column_map[MAX_COLUMNS];
    int value_columns = 0;
    p = skip_ws(p);
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
                snprintf(err, err_len, "constraint failed: unknown column '%s.%s'", table_name, col_name);
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
        value_columns = table->column_count;
        for (int i = 0; i < value_columns; i++) {
            column_map[i] = i;
        }
    }

    const char *values = ci_find(p, "values");
    if (values == NULL) {
        set_err(err, err_len, "expected VALUES");
        return -1;
    }
    p = values + 6;

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
        if (value_index != value_columns) {
            set_err(err, err_len, "column count and value count differ");
            return -1;
        }
        p++;
        batch->count++;
    }
    return 0;
}

static int parse_batch(const SharedState *state, const char *sql, InsertBatch *batch,
                       char *err, size_t err_len) {
    memset(batch, 0, sizeof(*batch));
    const char *start = sql;
    int in_string = 0;
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
            if (stmt[0] != '\0' && parse_insert_statement(state, stmt, batch, err, err_len) < 0) {
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

static int value_exists(const SharedState *state, const InsertBatch *batch, int upto,
                        int table_index, int col_index, const char *value) {
    const Table *table = &state->tables[table_index];
    for (int r = 0; r < table->row_count; r++) {
        if (!table->rows[r].is_null[col_index] &&
            strcmp(table->rows[r].values[col_index], value) == 0) {
            return 1;
        }
    }
    for (int i = 0; i < upto; i++) {
        if (batch->rows[i].table_index == table_index &&
            !batch->rows[i].row.is_null[col_index] &&
            strcmp(batch->rows[i].row.values[col_index], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int duplicate_value(const SharedState *state, const InsertBatch *batch, int upto,
                           int table_index, int col_index, const char *value) {
    return value_exists(state, batch, upto, table_index, col_index, value);
}

static int validate_batch_rows(const SharedState *state, const InsertBatch *batch,
                               char *err, size_t err_len) {
    for (int i = 0; i < batch->count; i++) {
        const InsertRow *ir = &batch->rows[i];
        const Table *table = &state->tables[ir->table_index];
        if (table->row_count >= MAX_ROWS) {
            snprintf(err, err_len, "constraint failed: table '%s' is full in demo storage", table->name);
            return -1;
        }
        for (int c = 0; c < table->column_count; c++) {
            const Column *col = &table->columns[c];
            if ((col->not_null || col->primary_key) && ir->row.is_null[c]) {
                snprintf(err, err_len, "constraint failed: %s.%s cannot be NULL", table->name, col->name);
                return -1;
            }
            if ((col->primary_key || col->unique) && !ir->row.is_null[c]) {
                if (duplicate_value(state, batch, i, ir->table_index, c, ir->row.values[c])) {
                    snprintf(err, err_len, "constraint failed: duplicate value for %s.%s",
                             table->name, col->name);
                    return -1;
                }
            }
            if (col->ref_table[0] != '\0' && !ir->row.is_null[c]) {
                int rt = find_table(state, col->ref_table);
                if (rt < 0) {
                    snprintf(err, err_len, "constraint failed: referenced table '%s' missing", col->ref_table);
                    return -1;
                }
                int rc = find_column(&state->tables[rt], col->ref_column);
                if (rc < 0) {
                    snprintf(err, err_len, "constraint failed: referenced column '%s.%s' missing",
                             col->ref_table, col->ref_column);
                    return -1;
                }
                if (!value_exists(state, batch, i, rt, rc, ir->row.values[c])) {
                    snprintf(err, err_len, "constraint failed: foreign key %s.%s references missing %s.%s=%s",
                             table->name, col->name, col->ref_table, col->ref_column, ir->row.values[c]);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int validate_insert_batch(const SharedState *state, const char *sql, char *err, size_t err_len) {
    if (state->table_count == 0) {
        set_err(err, err_len, "no ER diagram loaded");
        return -1;
    }
    InsertBatch batch;
    if (parse_batch(state, sql, &batch, err, err_len) < 0) {
        return -1;
    }
    if (validate_batch_rows(state, &batch, err, err_len) < 0) {
        return -1;
    }
    snprintf(err, err_len, "OK: %d row(s) accepted", batch.count);
    return 0;
}

int apply_insert_batch(SharedState *state, const char *sql, char *err, size_t err_len) {
    InsertBatch batch;
    if (parse_batch(state, sql, &batch, err, err_len) < 0) {
        return -1;
    }
    if (validate_batch_rows(state, &batch, err, err_len) < 0) {
        return -1;
    }
    for (int i = 0; i < batch.count; i++) {
        Table *table = &state->tables[batch.rows[i].table_index];
        table->rows[table->row_count++] = batch.rows[i].row;
    }
    snprintf(err, err_len, "OK: inserted %d row(s)", batch.count);
    return 0;
}

void admin_report(const SharedState *state, const char *category, char *out, size_t out_len) {
    out[0] = '\0';
    if (strcmp(category, "clients") == 0) {
        appendf(out, out_len, "Connected ordinary clients: %d\n", state->client_count);
        for (int i = 0; i < 128; i++) {
            if (state->clients[i].fd > 0) {
                appendf(out, out_len, "client=%d fd=%d addr=%s idle=%lds\n",
                        state->clients[i].client_id, state->clients[i].fd, state->clients[i].addr,
                        (long)(time(NULL) - state->clients[i].last_seen));
            }
        }
    } else if (strcmp(category, "commands") == 0) {
        appendf(out, out_len, "Total commands: %ld\nInsert commands: %ld\nFailed commands: %ld\n",
                state->total_commands, state->insert_commands, state->failed_commands);
    } else if (strcmp(category, "avg") == 0) {
        long avg = state->total_commands ? state->total_exec_ms / state->total_commands : 0;
        appendf(out, out_len, "Average execution time: %ld ms\n", avg);
    } else if (strcmp(category, "history") == 0) {
        appendf(out, out_len, "Recent history:\n");
        int start = state->history_count > MAX_HISTORY ? state->history_count - MAX_HISTORY : 0;
        for (int i = start; i < state->history_count; i++) {
            appendf(out, out_len, "%s\n", state->history[i % MAX_HISTORY]);
        }
    } else if (strcmp(category, "tables") == 0) {
        appendf(out, out_len, "Tables loaded: %d\n", state->table_count);
        for (int i = 0; i < state->table_count; i++) {
            appendf(out, out_len, "%s: %d columns, %d rows\n",
                    state->tables[i].name, state->tables[i].column_count, state->tables[i].row_count);
        }
    } else if (strcmp(category, "queue") == 0) {
        appendf(out, out_len, "Ordinary request queue depth: %d\n", state->queue_depth);
    } else {
        appendf(out, out_len, "Unknown category. Use: clients, commands, avg, history, tables, queue\n");
    }
}

#ifndef SQLCG_MODEL_H
#define SQLCG_MODEL_H

#include <stddef.h>

#define MAX_TABLES 32
#define MAX_COLUMNS 32
#define MAX_ROWS 1024
#define MAX_NAME 64
#define MAX_TYPE 64
#define MAX_VALUE 128
#define MAX_HISTORY 128
#define MAX_COMMAND 128

typedef struct {
    char name[MAX_NAME];
    char type[MAX_TYPE];
    int primary_key;
    int unique;
    int not_null;
    char ref_table[MAX_NAME];
    char ref_column[MAX_NAME];
} Column;

typedef struct {
    char values[MAX_COLUMNS][MAX_VALUE];
    int is_null[MAX_COLUMNS];
} Row;

typedef struct {
    char name[MAX_NAME];
    int column_count;
    Column columns[MAX_COLUMNS];
    int row_count;
    Row rows[MAX_ROWS];
} Table;

typedef struct {
    int client_id;
    int fd;
    char addr[64];
    long connected_at;
    long last_seen;
} ClientInfo;

typedef struct {
    long started_at;
    long total_commands;
    long insert_commands;
    long failed_commands;
    long total_exec_ms;
    int queue_depth;
    int client_count;
    int next_client_id;
    int admin_connected;
    ClientInfo clients[128];
    char history[MAX_HISTORY][MAX_COMMAND];
    int history_count;
    int table_count;
    Table tables[MAX_TABLES];
} SharedState;

void state_init(SharedState *state);
int load_er_json(SharedState *state, const char *path, char *err, size_t err_len);
int generate_sql(const SharedState *state, char *out, size_t out_len);
int validate_insert_batch(const SharedState *state, const char *sql, char *err, size_t err_len);
int apply_insert_batch(SharedState *state, const char *sql, char *err, size_t err_len);
void admin_report(const SharedState *state, const char *category, char *out, size_t out_len);

#endif

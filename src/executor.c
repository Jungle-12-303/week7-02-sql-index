#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlproc.h"

/*
 * executor.c는 SQL 문장 구조체를 실행 단계로 연결하는 모듈입니다.
 * - INSERT: 스키마/값 유효성을 확인한 뒤 스토리지에 행 추가를 요청합니다.
 * - SELECT: 선택 컬럼을 계산한 뒤 스토리지에 조회 출력을 요청합니다.
 */

#define EXECUTOR_MAX_TABLE_STATES 32

typedef struct {
    int in_use;
    char schema_dir[256];
    char data_dir[256];
    char table_name[SQLPROC_MAX_NAME_LEN];
    int next_id;
    BPlusTree *pk_index;
} TableRuntimeState;

static TableRuntimeState table_states[EXECUTOR_MAX_TABLE_STATES];

static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    /* SQL 문장 자체와 관련된 오류는 line/column 위치를 함께 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

static int find_schema_column(const TableSchema *schema, const char *name)
{
    int i;

    /* 컬럼 이름을 스키마 순서 인덱스로 바꿉니다. 찾지 못하면 -1입니다. */
    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static int validate_literal_type(DataType data_type, const LiteralValue *value)
{
    /* INSERT에서 리터럴 타입이 컬럼 타입과 맞는지 확인합니다. */
    if (data_type == DATA_TYPE_INT && value->type == LITERAL_INT) {
        return 1;
    }

    if (data_type == DATA_TYPE_STRING && value->type == LITERAL_STRING) {
        return 1;
    }

    return 0;
}

static double elapsed_ms(clock_t start_time, clock_t end_time)
{
    return ((double)(end_time - start_time) * 1000.0) / (double)CLOCKS_PER_SEC;
}

static int int_literal_in_range(const LiteralValue *value)
{
    char *end_ptr;
    long parsed_value;

    errno = 0;
    parsed_value = strtol(value->text, &end_ptr, 10);
    if (errno == ERANGE) {
        return 0;
    }

    if (*end_ptr != '\0') {
        return 0;
    }

    return parsed_value >= INT_MIN && parsed_value <= INT_MAX;
}

static int string_literal_is_safe_for_csv(const LiteralValue *value)
{
    char first_character;

    /*
     * CSV를 스프레드시트에서 열었을 때 수식으로 해석될 수 있는
     * 시작 문자(=, +, -, @)는 문자열 값으로 허용하지 않습니다.
     */
    if (value->text[0] == '\0') {
        return 1;
    }

    first_character = value->text[0];
    return first_character != '=' &&
           first_character != '+' &&
           first_character != '-' &&
           first_character != '@';
}

static int find_table_state(const AppConfig *config, const TableSchema *schema)
{
    int i;

    for (i = 0; i < EXECUTOR_MAX_TABLE_STATES; i++) {
        if (table_states[i].in_use &&
            strcmp(table_states[i].schema_dir, config->schema_dir) == 0 &&
            strcmp(table_states[i].data_dir, config->data_dir) == 0 &&
            strcmp(table_states[i].table_name, schema->table_name) == 0) {
            return i;
        }
    }

    return -1;
}

static int create_table_state(const AppConfig *config,
                              const TableSchema *schema,
                              ErrorInfo *error)
{
    int max_id;
    int i;
    BPlusTree *index;

    index = NULL;
    max_id = 0;

    if (schema->primary_key_index >= 0) {
        index = bptree_create();
        if (index == NULL) {
            set_runtime_error(error, "PK 인덱스를 만들 수 없습니다.", (SourceLocation){0, 0});
            return -1;
        }

        if (!storage_rebuild_pk_index(config, schema, index, &max_id, error)) {
            bptree_destroy(index);
            return -1;
        }
    }

    if (max_id == INT_MAX) {
        bptree_destroy(index);
        set_runtime_error(error, "자동 PK 값이 int 범위를 벗어났습니다.", (SourceLocation){0, 0});
        return -1;
    }

    for (i = 0; i < EXECUTOR_MAX_TABLE_STATES; i++) {
        if (!table_states[i].in_use) {
            table_states[i].in_use = 1;
            snprintf(table_states[i].schema_dir,
                     sizeof(table_states[i].schema_dir),
                     "%s",
                     config->schema_dir);
            snprintf(table_states[i].data_dir,
                     sizeof(table_states[i].data_dir),
                     "%s",
                     config->data_dir);
            snprintf(table_states[i].table_name,
                     sizeof(table_states[i].table_name),
                     "%s",
                     schema->table_name);
            table_states[i].next_id = max_id + 1;
            table_states[i].pk_index = index;
            return i;
        }
    }

    bptree_destroy(index);
    set_runtime_error(error, "테이블 상태 저장 공간이 부족합니다.", (SourceLocation){0, 0});
    return -1;
}

static int get_table_state_index(const AppConfig *config,
                                 const TableSchema *schema,
                                 ErrorInfo *error)
{
    int state_index;

    state_index = find_table_state(config, schema);
    if (state_index >= 0) {
        return state_index;
    }

    return create_table_state(config, schema, error);
}

static int allocate_next_primary_key(const AppConfig *config,
                                     const TableSchema *schema,
                                     int *next_id,
                                     ErrorInfo *error)
{
    int state_index;

    state_index = get_table_state_index(config, schema, error);
    if (state_index < 0) {
        return 0;
    }

    if (table_states[state_index].next_id == INT_MAX) {
        set_runtime_error(error, "자동 PK 값이 int 범위를 벗어났습니다.", (SourceLocation){0, 0});
        return 0;
    }

    *next_id = table_states[state_index].next_id;
    table_states[state_index].next_id += 1;
    return 1;
}

static void remember_explicit_primary_key(const AppConfig *config,
                                          const TableSchema *schema,
                                          int explicit_id)
{
    int state_index;

    state_index = find_table_state(config, schema);
    if (state_index < 0 || explicit_id == INT_MAX) {
        return;
    }

    if (table_states[state_index].next_id <= explicit_id) {
        table_states[state_index].next_id = explicit_id + 1;
    }
}

static int parse_row_primary_key(const TableSchema *schema,
                                 char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                 int *primary_key_value,
                                 ErrorInfo *error)
{
    char *end_ptr;
    long parsed_value;

    errno = 0;
    parsed_value = strtol(row_values[schema->primary_key_index], &end_ptr, 10);
    if (errno == ERANGE || *end_ptr != '\0' ||
        parsed_value < INT_MIN || parsed_value > INT_MAX) {
        set_runtime_error(error, "PK 값을 읽을 수 없습니다.", (SourceLocation){0, 0});
        return 0;
    }

    *primary_key_value = (int)parsed_value;
    return 1;
}

static int validate_primary_key_unique(const AppConfig *config,
                                       const TableSchema *schema,
                                       int primary_key_value,
                                       ErrorInfo *error)
{
    int exists;
    int state_index;
    long offset;

    exists = 0;
    state_index = get_table_state_index(config, schema, error);
    if (state_index < 0) {
        return 0;
    }

    if (table_states[state_index].pk_index != NULL &&
        bptree_search(table_states[state_index].pk_index, primary_key_value, &offset)) {
        exists = 1;
    }

    if (exists) {
        set_runtime_error(error, "PK 값이 이미 존재합니다.", (SourceLocation){0, 0});
        return 0;
    }

    return 1;
}

static int build_insert_row_values(const AppConfig *config,
                                   const TableSchema *schema,
                                   const InsertStatement *statement,
                                   char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   ErrorInfo *error)
{
    int used_columns[SQLPROC_MAX_COLUMNS];
    int i;

    /*
     * INSERT 구조체를 실제 "스키마 순서의 한 행 데이터"로 정렬합니다.
     * - 컬럼 목록 생략 시: 스키마 순서 그대로 값 매핑
     * - 컬럼 목록 명시 시: 이름을 찾아 해당 스키마 위치로 값 배치
     */
    memset(used_columns, 0, sizeof(used_columns));
    memset(row_values, 0, sizeof(char[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN]));

    if (!statement->has_column_list) {
        if (statement->value_count != schema->column_count) {
            set_runtime_error(error,
                              "VALUES 값 수가 스키마 컬럼 수와 일치하지 않습니다.",
                              statement->table_location);
            return 0;
        }

        for (i = 0; i < schema->column_count; i++) {
            if (!validate_literal_type(schema->columns[i].type, &statement->values[i])) {
                set_runtime_error(error,
                                  "INSERT 값 타입이 스키마와 맞지 않습니다.",
                                  statement->values[i].location);
                return 0;
            }

            if (schema->columns[i].type == DATA_TYPE_INT &&
                !int_literal_in_range(&statement->values[i])) {
                set_runtime_error(error,
                                  "정수 값이 int 범위를 벗어났습니다.",
                                  statement->values[i].location);
                return 0;
            }

            if (schema->columns[i].type == DATA_TYPE_STRING &&
                !string_literal_is_safe_for_csv(&statement->values[i])) {
                set_runtime_error(error,
                                  "문자열 값이 CSV에서 수식으로 해석될 수 없습니다.",
                                  statement->values[i].location);
                return 0;
            }

            snprintf(row_values[i], sizeof(row_values[i]), "%s", statement->values[i].text);

        }

        return 1;
    }

    for (i = 0; i < statement->column_count; i++) {
        int schema_index;

        schema_index = find_schema_column(schema, statement->column_names[i]);
        if (schema_index < 0) {
            set_runtime_error(error,
                              "INSERT 대상 컬럼이 스키마에 없습니다.",
                              statement->column_locations[i]);
            return 0;
        }

        if (used_columns[schema_index]) {
            set_runtime_error(error,
                              "같은 컬럼이 INSERT 문에 두 번 들어왔습니다.",
                              statement->column_locations[i]);
            return 0;
        }

        if (!validate_literal_type(schema->columns[schema_index].type,
                                   &statement->values[i])) {
            set_runtime_error(error,
                              "INSERT 값 타입이 스키마와 맞지 않습니다.",
                              statement->values[i].location);
            return 0;
        }

        if (schema->columns[schema_index].type == DATA_TYPE_INT &&
            !int_literal_in_range(&statement->values[i])) {
            set_runtime_error(error,
                              "정수 값이 int 범위를 벗어났습니다.",
                              statement->values[i].location);
            return 0;
        }

        if (schema->columns[schema_index].type == DATA_TYPE_STRING &&
            !string_literal_is_safe_for_csv(&statement->values[i])) {
            set_runtime_error(error,
                              "문자열 값이 CSV에서 수식으로 해석될 수 없습니다.",
                              statement->values[i].location);
            return 0;
        }

        snprintf(row_values[schema_index], sizeof(row_values[schema_index]), "%s",
                 statement->values[i].text);
        used_columns[schema_index] = 1;

    }

    for (i = 0; i < schema->column_count; i++) {
        if (!used_columns[i]) {
            if (i == schema->primary_key_index) {
                int next_id;

                if (!allocate_next_primary_key(config, schema, &next_id, error)) {
                    return 0;
                }

                snprintf(row_values[i], sizeof(row_values[i]), "%d", next_id);
                used_columns[i] = 1;
                continue;
            }

            set_runtime_error(error,
                              "INSERT 문에 스키마의 모든 컬럼 값이 필요합니다.",
                              statement->table_location);
            return 0;
        }
    }

    return 1;
}

static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    long row_offset;
    int primary_key_value;
    int has_primary_key;

    /*
     * INSERT 실행 흐름:
     * 1. 스키마 로드
     * 2. 구조체 값을 스키마 순서 행 데이터로 정리
     * 3. 스토리지에 append 요청
     */
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    if (!build_insert_row_values(config, &schema, statement, row_values, error)) {
        return 0;
    }

    has_primary_key = schema.primary_key_index >= 0;
    primary_key_value = 0;
    if (has_primary_key) {
        if (!parse_row_primary_key(&schema, row_values, &primary_key_value, error)) {
            return 0;
        }

        if (!validate_primary_key_unique(config, &schema, primary_key_value, error)) {
            return 0;
        }
    }

    if (!storage_append_row(config, &schema, row_values, &row_offset, error)) {
        return 0;
    }

    if (has_primary_key) {
        int state_index;

        state_index = get_table_state_index(config, &schema, error);
        if (state_index < 0) {
            return 0;
        }

        if (!bptree_insert(table_states[state_index].pk_index, primary_key_value, row_offset)) {
            set_runtime_error(error, "PK 인덱스에 값을 등록할 수 없습니다.", (SourceLocation){0, 0});
            return 0;
        }

        remember_explicit_primary_key(config, &schema, primary_key_value);
    }

    return 1;
}

static int resolve_selected_columns(const TableSchema *schema,
                                    const SelectStatement *statement,
                                    int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int *selected_count,
                                    ErrorInfo *error)
{
    int i;

    /* SELECT * 또는 SELECT col1, col2 를 실제 스키마 인덱스 배열로 바꿉니다. */
    *selected_count = 0;

    if (statement->select_all) {
        for (i = 0; i < schema->column_count; i++) {
            selected_indices[*selected_count] = i;
            *selected_count += 1;
        }
    } else {
        for (i = 0; i < statement->column_count; i++) {
            int column_index;

            column_index = find_schema_column(schema, statement->column_names[i]);
            if (column_index < 0) {
                set_runtime_error(error,
                                  "SELECT 대상 컬럼이 스키마에 없습니다.",
                                  statement->column_locations[i]);
                *selected_count = 0;
                return 0;
            }

            selected_indices[*selected_count] = column_index;
            *selected_count += 1;
        }
    }

    return 1;
}

static int resolve_where_column(const TableSchema *schema,
                                const SelectStatement *statement,
                                int *where_column_index,
                                ErrorInfo *error)
{
    *where_column_index = find_schema_column(schema, statement->where_column);
    if (*where_column_index < 0) {
        set_runtime_error(error,
                          "WHERE 대상 컬럼이 스키마에 없습니다.",
                          statement->where_column_location);
        return 0;
    }

    if (!validate_literal_type(schema->columns[*where_column_index].type,
                               &statement->where_value)) {
        set_runtime_error(error,
                          "WHERE 값 타입이 스키마와 맞지 않습니다.",
                          statement->where_value.location);
        return 0;
    }

    if (schema->columns[*where_column_index].type == DATA_TYPE_INT &&
        !int_literal_in_range(&statement->where_value)) {
        set_runtime_error(error,
                          "정수 값이 int 범위를 벗어났습니다.",
                          statement->where_value.location);
        return 0;
    }

    if (schema->columns[*where_column_index].type == DATA_TYPE_STRING &&
        (statement->where_operator == WHERE_OP_GREATER ||
         statement->where_operator == WHERE_OP_LESS)) {
        set_runtime_error(error,
                          "문자열 컬럼은 >, < 조건을 사용할 수 없습니다.",
                          statement->where_value.location);
        return 0;
    }

    return 1;
}

static int parse_where_int_value(const LiteralValue *value, int *out_value)
{
    char *end_ptr;
    long parsed_value;

    errno = 0;
    parsed_value = strtol(value->text, &end_ptr, 10);
    if (errno == ERANGE || *end_ptr != '\0' ||
        parsed_value < INT_MIN || parsed_value > INT_MAX) {
        return 0;
    }

    *out_value = (int)parsed_value;
    return 1;
}

static int execute_select_with_index(const AppConfig *config,
                                     const TableSchema *schema,
                                     const SelectStatement *statement,
                                     const int selected_indices[SQLPROC_MAX_COLUMNS],
                                     int selected_count,
                                     ErrorInfo *error)
{
    int state_index;
    int target_id;
    long row_offset;

    if (!parse_where_int_value(&statement->where_value, &target_id)) {
        set_runtime_error(error,
                          "정수 값이 int 범위를 벗어났습니다.",
                          statement->where_value.location);
        return 0;
    }

    state_index = get_table_state_index(config, schema, error);
    if (state_index < 0) {
        return 0;
    }

    printf("[INDEX] WHERE %s = %s\n",
           statement->where_column,
           statement->where_value.text);

    if (!bptree_search(table_states[state_index].pk_index, target_id, &row_offset)) {
        row_offset = -1;
    }

    return storage_print_row_at_offset(config,
                                       schema,
                                       row_offset,
                                       selected_indices,
                                       selected_count,
                                       error);
}

typedef struct {
    long *offsets;
    int count;
    int capacity;
} OffsetList;

static int append_offset(int key, long offset, void *user_data)
{
    OffsetList *list;
    long *next_offsets;
    int next_capacity;

    (void)key;
    list = (OffsetList *)user_data;

    if (list->count == list->capacity) {
        next_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        next_offsets = (long *)realloc(list->offsets, sizeof(long) * (size_t)next_capacity);
        if (next_offsets == NULL) {
            return 0;
        }

        list->offsets = next_offsets;
        list->capacity = next_capacity;
    }

    list->offsets[list->count] = offset;
    list->count += 1;
    return 1;
}

static const char *where_operator_text(WhereOperator where_operator)
{
    if (where_operator == WHERE_OP_EQUAL) {
        return "=";
    }

    if (where_operator == WHERE_OP_GREATER) {
        return ">";
    }

    if (where_operator == WHERE_OP_LESS) {
        return "<";
    }

    return "!=";
}

static int execute_select_with_index_range(const AppConfig *config,
                                           const TableSchema *schema,
                                           const SelectStatement *statement,
                                           const int selected_indices[SQLPROC_MAX_COLUMNS],
                                           int selected_count,
                                           ErrorInfo *error)
{
    OffsetList offsets;
    int state_index;
    int target_id;
    int ok;

    if (!parse_where_int_value(&statement->where_value, &target_id)) {
        set_runtime_error(error,
                          "정수 값이 int 범위를 벗어났습니다.",
                          statement->where_value.location);
        return 0;
    }

    state_index = get_table_state_index(config, schema, error);
    if (state_index < 0) {
        return 0;
    }

    memset(&offsets, 0, sizeof(offsets));
    if (statement->where_operator == WHERE_OP_GREATER) {
        ok = bptree_visit_greater_than(table_states[state_index].pk_index,
                                       target_id,
                                       append_offset,
                                       &offsets);
    } else {
        ok = bptree_visit_less_than(table_states[state_index].pk_index,
                                    target_id,
                                    append_offset,
                                    &offsets);
    }

    if (!ok) {
        free(offsets.offsets);
        set_runtime_error(error, "PK 인덱스 range scan 중 오류가 발생했습니다.", (SourceLocation){0, 0});
        return 0;
    }

    printf("[INDEX-RANGE] WHERE %s %s %s (%d rows)\n",
           statement->where_column,
           where_operator_text(statement->where_operator),
           statement->where_value.text,
           offsets.count);

    ok = storage_print_rows_at_offsets(config,
                                       schema,
                                       offsets.offsets,
                                       offsets.count,
                                       selected_indices,
                                       selected_count,
                                       error);
    free(offsets.offsets);
    return ok;
}

static int execute_select_with_scan(const AppConfig *config,
                                    const TableSchema *schema,
                                    const SelectStatement *statement,
                                    const int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int selected_count,
                                    int where_column_index,
                                    ErrorInfo *error)
{
    printf("[SCAN] WHERE %s %s %s\n",
           statement->where_column,
           where_operator_text(statement->where_operator),
           statement->where_value.text);

    return storage_print_rows_where_equals(config,
                                           schema,
                                           selected_indices,
                                           selected_count,
                                           where_column_index,
                                           statement->where_operator,
                                           &statement->where_value,
                                           error);
}

static int execute_select(const AppConfig *config,
                          const SelectStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    int selected_count;
    int where_column_index;
    int use_index_lookup;
    int use_index_range;
    clock_t start_time;
    clock_t end_time;
    int result;

    /*
     * SELECT 실행 흐름:
     * 1. 스키마/선택 컬럼 유효성 확인
     * 2. 스토리지에 조회 출력 요청
     */
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    if (!resolve_selected_columns(&schema,
                                  statement,
                                  selected_indices,
                                  &selected_count,
                                  error)) {
        return 0;
    }

    if (!statement->has_where) {
        start_time = clock();
        result = storage_print_rows(config, &schema, selected_indices, selected_count, error);
        end_time = clock();
        if (result) {
            printf("elapsed: %.3f ms\n",
                   ((double)(end_time - start_time) * 1000.0) / (double)CLOCKS_PER_SEC);
        }
        return result;
    }

    if (!resolve_where_column(&schema, statement, &where_column_index, error)) {
        return 0;
    }

    use_index_lookup = schema.primary_key_index >= 0 &&
                       where_column_index == schema.primary_key_index &&
                       statement->where_value.type == LITERAL_INT &&
                       statement->where_operator == WHERE_OP_EQUAL;
    use_index_range = schema.primary_key_index >= 0 &&
                      where_column_index == schema.primary_key_index &&
                      statement->where_value.type == LITERAL_INT &&
                      (statement->where_operator == WHERE_OP_GREATER ||
                       statement->where_operator == WHERE_OP_LESS);

    start_time = clock();
    if (use_index_lookup || use_index_range) {
        if (get_table_state_index(config, &schema, error) < 0) {
            return 0;
        }
    }

    if (use_index_lookup) {
        result = execute_select_with_index(config,
                                           &schema,
                                           statement,
                                           selected_indices,
                                           selected_count,
                                           error);
        end_time = clock();
        if (result) {
            printf("elapsed: %.3f ms\n", elapsed_ms(start_time, end_time));
        }
        return result;
    }

    if (use_index_range) {
        result = execute_select_with_index_range(config,
                                                 &schema,
                                                 statement,
                                                 selected_indices,
                                                 selected_count,
                                                 error);
        end_time = clock();
        if (result) {
            printf("elapsed: %.3f ms\n", elapsed_ms(start_time, end_time));
        }
        return result;
    }

    result = execute_select_with_scan(config,
                                      &schema,
                                      statement,
                                      selected_indices,
                                      selected_count,
                                      where_column_index,
                                      error);
    end_time = clock();
    if (result) {
        printf("elapsed: %.3f ms\n", elapsed_ms(start_time, end_time));
    }
    return result;
}

int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error)
{
    int i;

    /* SqlProgram에 담긴 문장들을 앞에서부터 순서대로 실행합니다. */
    memset(error, 0, sizeof(*error));

    for (i = 0; i < program->count; i++) {
        if (program->items[i].type == STATEMENT_INSERT) {
            if (!execute_insert(config, &program->items[i].insert_statement, error)) {
                return 0;
            }
            continue;
        }

        if (!execute_select(config, &program->items[i].select_statement, error)) {
            return 0;
        }
    }

    return 1;
}

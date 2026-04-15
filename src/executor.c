#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

/*
 * executor.c는 SQL 문장 구조체를 실행 단계로 연결하는 모듈입니다.
 * - INSERT: 스키마/값 유효성을 확인한 뒤 스토리지에 행 추가를 요청합니다.
 * - SELECT: 선택 컬럼을 계산한 뒤 스토리지에 조회 출력을 요청합니다.
 */

#define EXECUTOR_MAX_PK_STATES 32

typedef struct {
    int in_use;
    char schema_dir[256];
    char data_dir[256];
    char table_name[SQLPROC_MAX_NAME_LEN];
    int next_id;
} PrimaryKeyState;

static PrimaryKeyState primary_key_states[EXECUTOR_MAX_PK_STATES];

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

static int find_primary_key_state(const AppConfig *config, const TableSchema *schema)
{
    int i;

    for (i = 0; i < EXECUTOR_MAX_PK_STATES; i++) {
        if (primary_key_states[i].in_use &&
            strcmp(primary_key_states[i].schema_dir, config->schema_dir) == 0 &&
            strcmp(primary_key_states[i].data_dir, config->data_dir) == 0 &&
            strcmp(primary_key_states[i].table_name, schema->table_name) == 0) {
            return i;
        }
    }

    return -1;
}

static int create_primary_key_state(const AppConfig *config,
                                    const TableSchema *schema,
                                    ErrorInfo *error)
{
    int max_id;
    int i;

    if (!storage_find_max_int_value(config,
                                    schema,
                                    schema->primary_key_index,
                                    &max_id,
                                    error)) {
        return -1;
    }

    if (max_id == INT_MAX) {
        set_runtime_error(error, "자동 PK 값이 int 범위를 벗어났습니다.", (SourceLocation){0, 0});
        return -1;
    }

    for (i = 0; i < EXECUTOR_MAX_PK_STATES; i++) {
        if (!primary_key_states[i].in_use) {
            primary_key_states[i].in_use = 1;
            snprintf(primary_key_states[i].schema_dir,
                     sizeof(primary_key_states[i].schema_dir),
                     "%s",
                     config->schema_dir);
            snprintf(primary_key_states[i].data_dir,
                     sizeof(primary_key_states[i].data_dir),
                     "%s",
                     config->data_dir);
            snprintf(primary_key_states[i].table_name,
                     sizeof(primary_key_states[i].table_name),
                     "%s",
                     schema->table_name);
            primary_key_states[i].next_id = max_id + 1;
            return i;
        }
    }

    set_runtime_error(error, "PK 상태 저장 공간이 부족합니다.", (SourceLocation){0, 0});
    return -1;
}

static int get_primary_key_state_index(const AppConfig *config,
                                       const TableSchema *schema,
                                       ErrorInfo *error)
{
    int state_index;

    state_index = find_primary_key_state(config, schema);
    if (state_index >= 0) {
        return state_index;
    }

    return create_primary_key_state(config, schema, error);
}

static int allocate_next_primary_key(const AppConfig *config,
                                     const TableSchema *schema,
                                     int *next_id,
                                     ErrorInfo *error)
{
    int state_index;

    state_index = get_primary_key_state_index(config, schema, error);
    if (state_index < 0) {
        return 0;
    }

    if (primary_key_states[state_index].next_id == INT_MAX) {
        set_runtime_error(error, "자동 PK 값이 int 범위를 벗어났습니다.", (SourceLocation){0, 0});
        return 0;
    }

    *next_id = primary_key_states[state_index].next_id;
    primary_key_states[state_index].next_id += 1;
    return 1;
}

static void remember_explicit_primary_key(const AppConfig *config,
                                          const TableSchema *schema,
                                          int explicit_id)
{
    int state_index;

    state_index = find_primary_key_state(config, schema);
    if (state_index < 0 || explicit_id == INT_MAX) {
        return;
    }

    if (primary_key_states[state_index].next_id <= explicit_id) {
        primary_key_states[state_index].next_id = explicit_id + 1;
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

    if (!storage_int_value_exists(config,
                                  schema,
                                  schema->primary_key_index,
                                  primary_key_value,
                                  &exists,
                                  error)) {
        return 0;
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
    int explicit_primary_key;
    int i;

    /*
     * INSERT 구조체를 실제 "스키마 순서의 한 행 데이터"로 정렬합니다.
     * - 컬럼 목록 생략 시: 스키마 순서 그대로 값 매핑
     * - 컬럼 목록 명시 시: 이름을 찾아 해당 스키마 위치로 값 배치
     */
    memset(used_columns, 0, sizeof(used_columns));
    memset(row_values, 0, sizeof(char[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN]));
    explicit_primary_key = 0;

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

            if (i == schema->primary_key_index) {
                explicit_primary_key = atoi(statement->values[i].text);
            }
        }

        if (schema->primary_key_index >= 0) {
            remember_explicit_primary_key(config, schema, explicit_primary_key);
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

        if (schema_index == schema->primary_key_index) {
            explicit_primary_key = atoi(statement->values[i].text);
        }
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

    if (schema->primary_key_index >= 0 &&
        used_columns[schema->primary_key_index] &&
        explicit_primary_key > 0) {
        remember_explicit_primary_key(config, schema, explicit_primary_key);
    }

    return 1;
}

static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];

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

    if (schema.primary_key_index >= 0) {
        int primary_key_value;

        if (!parse_row_primary_key(&schema, row_values, &primary_key_value, error)) {
            return 0;
        }

        if (!validate_primary_key_unique(config, &schema, primary_key_value, error)) {
            return 0;
        }
    }

    return storage_append_row(config, &schema, row_values, error);
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
static int execute_select(const AppConfig *config,
                          const SelectStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    int selected_count;

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

    return storage_print_rows(config, &schema, selected_indices, selected_count, error);
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

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bptree.h"
#include "sqlproc.h"

/*
 * test_runner.c는 프로젝트의 통합 테스트와 단위 성격 테스트를 함께 담습니다.
 * 초심자가 흐름을 따라가기 쉽도록 한 파일에서
 * - 인자 파싱
 * - 토크나이저/파서
 * - CSV 기반 스토리지/실행기
 * 를 순서대로 검증합니다.
 */

static int ensure_directory(const char *path);
static int write_text_file(const char *path, const char *text);
static int write_generated_users_csv(const char *path, int record_count);
static int read_first_two_elapsed_ms(const char *path, double *first_ms, double *second_ms);
static int file_contains_text(const char *path, const char *needle);
static int count_text_occurrences(const char *path, const char *needle);
static int path_exists(const char *path);
static int capture_run_program(const AppConfig *config, const char *output_path);
static int capture_run_program_with_input(const AppConfig *config,
                                          const char *input_text,
                                          const char *output_path);
static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
                                      const char *output_path,
                                      ErrorInfo *error);
static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 const char *prefix);
static int parse_sql_text(const char *sql_text, SqlProgram *program, ErrorInfo *error);
static int make_temp_directory(char *path_template);

/* mkdtemp 선언 가용성에 기대지 않고, 고유한 임시 경로를 디렉터리로 바꿉니다. */
static int make_temp_directory(char *path_template)
{
    int file_descriptor;

    file_descriptor = mkstemp(path_template);
    if (file_descriptor < 0) {
        return 0;
    }

    close(file_descriptor);
    if (unlink(path_template) != 0) {
        return 0;
    }

    return mkdir(path_template, 0700) == 0;
}

static int test_parse_arguments_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "input.sql"
    };

    if (!parse_arguments(6, argv, &config)) {
        return 0;
    }

    if (strcmp(config.schema_dir, "schemas") != 0) {
        return 0;
    }

    if (strcmp(config.data_dir, "data") != 0) {
        return 0;
    }

    if (strcmp(config.input_path, "input.sql") != 0) {
        return 0;
    }

    return 1;
}

static int test_parse_arguments_fail(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "input.sql"
    };

    return !parse_arguments(5, argv, &config);
}

static int test_parse_arguments_benchmark_short_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "-b"
    };

    if (!parse_arguments(6, argv, &config)) {
        return 0;
    }

    return strcmp(config.schema_dir, "schemas") == 0 &&
           strcmp(config.data_dir, "data") == 0 &&
           config.benchmark_mode == 1 &&
           config.interactive_mode == 0 &&
           config.input_path[0] == '\0';
}

static int test_parse_arguments_benchmark_long_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--data-dir", "data",
        "--benchmark",
        "--schema-dir", "schemas"
    };

    if (!parse_arguments(6, argv, &config)) {
        return 0;
    }

    return strcmp(config.schema_dir, "schemas") == 0 &&
           strcmp(config.data_dir, "data") == 0 &&
           config.benchmark_mode == 1 &&
           config.interactive_mode == 0 &&
           config.input_path[0] == '\0';
}

static int test_parse_arguments_reject_mixed_modes(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "--benchmark",
        "input.sql"
    };

    return !parse_arguments(7, argv, &config);
}

static int test_tokenize_select(void)
{
    TokenList tokens;
    ErrorInfo error;

    if (!tokenize_sql("SELECT name FROM users;", &tokens, &error)) {
        return 0;
    }

    if (tokens.count != 6) {
        return 0;
    }

    if (tokens.items[0].type != TOKEN_KEYWORD_SELECT) {
        return 0;
    }

    if (tokens.items[1].type != TOKEN_IDENTIFIER ||
        strcmp(tokens.items[1].text, "name") != 0) {
        return 0;
    }

    if (tokens.items[3].type != TOKEN_IDENTIFIER ||
        strcmp(tokens.items[3].text, "users") != 0) {
        return 0;
    }

    if (tokens.items[4].type != TOKEN_SEMICOLON) {
        return 0;
    }

    return tokens.items[5].type == TOKEN_EOF;
}

static int test_tokenize_multiline_string_fail(void)
{
    TokenList tokens;
    ErrorInfo error;

    if (tokenize_sql("INSERT INTO users VALUES (1, 'hello\nworld', 20);", &tokens, &error)) {
        return 0;
    }

    if (strstr(error.message, "줄바꿈") == NULL) {
        return 0;
    }

    return error.line == 1 && error.column == 36;
}

static int test_parse_insert_statement(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users (id, name) VALUES (1, 'kim');", &tokens, &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.count != 1) {
        return 0;
    }

    if (program.items[0].type != STATEMENT_INSERT) {
        return 0;
    }

    if (strcmp(program.items[0].insert_statement.table_name, "users") != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.column_count != 2) {
        return 0;
    }

    if (strcmp(program.items[0].insert_statement.column_names[1], "name") != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.values[0].type != LITERAL_INT) {
        return 0;
    }

    if (program.items[0].insert_statement.values[1].location.line != 1) {
        return 0;
    }

    if (program.items[0].insert_statement.values[1].location.column <= 0) {
        return 0;
    }

    return strcmp(program.items[0].insert_statement.values[1].text, "kim") == 0;
}

static int test_parse_insert_without_column_list(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users VALUES (1, 'park', 40);", &tokens, &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.items[0].insert_statement.has_column_list) {
        return 0;
    }

    if (program.items[0].insert_statement.column_count != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.value_count != 3) {
        return 0;
    }

    return strcmp(program.items[0].insert_statement.values[1].text, "park") == 0;
}

static int test_parse_select_where_equals(void)
{
    SqlProgram program;
    ErrorInfo error;

    if (!parse_sql_text("SELECT name FROM users WHERE id = 7;", &program, &error)) {
        return 0;
    }

    if (program.count != 1 ||
        program.items[0].type != STATEMENT_SELECT) {
        return 0;
    }

    if (!program.items[0].select_statement.has_where) {
        return 0;
    }

    if (strcmp(program.items[0].select_statement.where_column, "id") != 0) {
        return 0;
    }

    if (program.items[0].select_statement.where_value.type != LITERAL_INT) {
        return 0;
    }

    return strcmp(program.items[0].select_statement.where_value.text, "7") == 0;
}

static int test_parse_select_where_greater(void)
{
    SqlProgram program;
    ErrorInfo error;

    if (!parse_sql_text("SELECT * FROM users WHERE id > 10;", &program, &error)) {
        return 0;
    }

    return program.items[0].select_statement.has_where &&
           program.items[0].select_statement.where_operator == WHERE_OP_GREATER &&
           strcmp(program.items[0].select_statement.where_column, "id") == 0 &&
           strcmp(program.items[0].select_statement.where_value.text, "10") == 0;
}

static int test_run_program_success(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char path[256];
    char output_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_run_program_success_")) {
        return 0;
    }

    snprintf(path, sizeof(path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    {
        char schema_path[512];
        char data_path[512];

        snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
        snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

        if (!write_text_file(schema_path, "id:int,name:string\n")) {
            return 0;
        }

        if (!write_text_file(data_path, "id,name\n1,kim\n")) {
            return 0;
        }
    }

    if (!write_text_file(path, "SELECT * FROM users;")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!file_contains_text(output_path, "id\tname\n1\tkim\n")) {
        return 0;
    }

    return 1;
}

static int ensure_directory(const char *path)
{
    /* 테스트용 임시 워크스페이스 디렉터리를 보장합니다. */
    if (mkdir(path, 0777) == 0) {
        return 1;
    }

    return access(path, F_OK) == 0;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file;

    /* 테스트 입력 SQL, 스키마, CSV 파일을 간단히 만들기 위한 헬퍼입니다. */
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    fputs(text, file);
    fclose(file);
    return 1;
}

static int write_generated_users_csv(const char *path, int record_count)
{
    FILE *file;
    int i;

    /* indexed SELECT 성능 회귀 테스트용으로 큰 users.csv를 빠르게 만듭니다. */
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    if (fputs("id,name,age\n", file) == EOF) {
        fclose(file);
        return 0;
    }

    for (i = 1; i <= record_count; i++) {
        if (fprintf(file, "%d,user%d,%d\n", i, i, 20 + (i % 50)) < 0) {
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

static int read_first_two_elapsed_ms(const char *path, double *first_ms, double *second_ms)
{
    FILE *file;
    char line[256];
    double value;
    int found_count;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    *first_ms = 0.0;
    *second_ms = 0.0;
    found_count = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "elapsed: %lf ms", &value) != 1) {
            continue;
        }

        if (found_count == 0) {
            *first_ms = value;
        } else {
            *second_ms = value;
            fclose(file);
            return 1;
        }

        found_count += 1;
    }

    fclose(file);
    return 0;
}

static int file_contains_text(const char *path, const char *needle)
{
    char buffer[2048];
    FILE *file;
    size_t size;

    /* 출력 파일에 특정 문자열이 포함됐는지 확인합니다. */
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strstr(buffer, needle) != NULL;
}

static int count_text_occurrences(const char *path, const char *needle)
{
    char buffer[8192];
    char *cursor;
    FILE *file;
    size_t size;
    int count;
    size_t needle_length;

    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }

    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';

    count = 0;
    needle_length = strlen(needle);
    cursor = buffer;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count += 1;
        cursor += needle_length;
    }

    return count;
}

static int path_exists(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

static int capture_run_program(const AppConfig *config, const char *output_path)
{
    FILE *file;
    int saved_stdout;
    int result;

    /* run_program의 stdout을 파일로 받아 SELECT 결과를 검증할 때 사용합니다. */
    file = fopen(output_path, "wb");
    if (file == NULL) {
        return 0;
    }

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        fclose(file);
        return 0;
    }

    if (dup2(fileno(file), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        fclose(file);
        return 0;
    }

    result = run_program(config);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result == 0;
}

static int capture_run_program_with_input(const AppConfig *config,
                                          const char *input_text,
                                          const char *output_path)
{
    char input_path[] = "/tmp/sqlproc_stdin_XXXXXX";
    FILE *input_file;
    FILE *output_file;
    int input_descriptor;
    int saved_stdin;
    int saved_stdout;
    int result;

    input_descriptor = mkstemp(input_path);
    if (input_descriptor < 0) {
        return 0;
    }

    input_file = fdopen(input_descriptor, "wb");
    if (input_file == NULL) {
        close(input_descriptor);
        unlink(input_path);
        return 0;
    }

    if (fputs(input_text, input_file) == EOF) {
        fclose(input_file);
        unlink(input_path);
        return 0;
    }

    fclose(input_file);
    input_file = fopen(input_path, "rb");
    if (input_file == NULL) {
        unlink(input_path);
        return 0;
    }

    output_file = fopen(output_path, "wb");
    if (output_file == NULL) {
        fclose(input_file);
        unlink(input_path);
        return 0;
    }

    fflush(stdout);
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        if (saved_stdin >= 0) {
            close(saved_stdin);
        }
        if (saved_stdout >= 0) {
            close(saved_stdout);
        }
        fclose(input_file);
        fclose(output_file);
        unlink(input_path);
        return 0;
    }

    if (dup2(fileno(input_file), STDIN_FILENO) < 0 ||
        dup2(fileno(output_file), STDOUT_FILENO) < 0) {
        close(saved_stdin);
        close(saved_stdout);
        fclose(input_file);
        fclose(output_file);
        unlink(input_path);
        return 0;
    }

    result = run_program(config);
    fflush(stdout);
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    fclose(input_file);
    fclose(output_file);
    unlink(input_path);
    return result == 0;
}

static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
                                      const char *output_path,
                                      ErrorInfo *error)
{
    FILE *file;
    int saved_stdout;
    int result;

    /* storage_print_rows의 stdout을 파일로 받아 헤더/조회 출력을 검증합니다. */
    file = fopen(output_path, "wb");
    if (file == NULL) {
        return 0;
    }

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        fclose(file);
        return 0;
    }

    if (dup2(fileno(file), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        fclose(file);
        return 0;
    }

    result = storage_print_rows(config, schema, selected_indices, selected_count, error);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result;
}

static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 const char *prefix)
{
    /* 각 테스트가 서로 간섭하지 않도록 /tmp 아래에 독립 워크스페이스를 만듭니다. */
    snprintf(base_path, base_size, "/tmp/%sXXXXXX", prefix);
    if (!make_temp_directory(base_path)) {
        return 0;
    }

    snprintf(schema_dir, schema_size, "%s/schemas", base_path);
    snprintf(data_dir, data_size, "%s/data", base_path);

    return ensure_directory(schema_dir) && ensure_directory(data_dir);
}

static int parse_sql_text(const char *sql_text, SqlProgram *program, ErrorInfo *error)
{
    TokenList tokens;

    if (!tokenize_sql(sql_text, &tokens, error)) {
        return 0;
    }

    return parse_program(&tokens, program, error);
}

static int test_bptree_single_key_search(void)
{
    BPlusTree *tree;
    long offset;
    int ok;

    tree = bptree_create();
    if (tree == NULL) {
        return 0;
    }

    ok = bptree_insert(tree, 10, 1234L) &&
         bptree_search(tree, 10, &offset) &&
         offset == 1234L &&
         !bptree_search(tree, 99, &offset);

    bptree_destroy(tree);
    return ok;
}

static int test_bptree_multiple_keys_and_split(void)
{
    BPlusTree *tree;
    int keys[] = {5, 2, 8, 1, 3, 7, 6, 4, 9, 10};
    int i;
    int ok;

    tree = bptree_create();
    if (tree == NULL) {
        return 0;
    }

    ok = 1;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        if (!bptree_insert(tree, keys[i], keys[i] * 100L)) {
            ok = 0;
            break;
        }
    }

    for (i = 1; ok && i <= 10; i++) {
        long offset;

        if (!bptree_search(tree, i, &offset) || offset != i * 100L) {
            ok = 0;
        }
    }

    bptree_destroy(tree);
    return ok;
}

static int test_bptree_duplicate_key_fail(void)
{
    BPlusTree *tree;
    long offset;
    int ok;

    tree = bptree_create();
    if (tree == NULL) {
        return 0;
    }

    ok = bptree_insert(tree, 1, 10L) &&
         !bptree_insert(tree, 1, 20L) &&
         bptree_search(tree, 1, &offset) &&
         offset == 10L;

    bptree_destroy(tree);
    return ok;
}

static int test_bptree_thousand_keys(void)
{
    BPlusTree *tree;
    int i;
    int ok;

    tree = bptree_create();
    if (tree == NULL) {
        return 0;
    }

    ok = 1;
    for (i = 1000; i >= 1; i--) {
        if (!bptree_insert(tree, i, i + 5000L)) {
            ok = 0;
            break;
        }
    }

    for (i = 1; ok && i <= 1000; i++) {
        long offset;

        if (!bptree_search(tree, i, &offset) || offset != i + 5000L) {
            ok = 0;
        }
    }

    bptree_destroy(tree);
    return ok;
}

static int test_parse_empty_sql_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("", &tokens, &error)) {
        return 0;
    }

    if (parse_program(&tokens, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "비어") != NULL;
}

static int test_insert_and_select_execution(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char data_path[512];
    char sql_path[256];
    char output_path[512];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_insert_select_test_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);"
                         "SELECT name, age FROM users;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!file_contains_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n")) {
        return 0;
    }

    return file_contains_text(output_path, "name\tage\nkim\t20\nlee\t30\n");
}

static int test_storage_print_rows_without_data_file(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char output_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_header_only_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (!capture_storage_print_rows(&config,
                                    &schema,
                                    selected_indices,
                                    2,
                                    output_path,
                                    &error)) {
        return 0;
    }

    return file_contains_text(output_path, "id\tname\n");
}

static int test_storage_print_rows_header_mismatch(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_bad_header_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "name,id\nkim,1\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (storage_print_rows(&config, &schema, selected_indices, 2, &error)) {
        return 0;
    }

    return strstr(error.message, "CSV 헤더 순서가 스키마와 다릅니다.") != NULL;
}

static int test_insert_int_overflow_fail(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_int_overflow_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users VALUES (9999999999999999999999999999999999999999, 'huge', 1);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    if (strstr(error.message, "int 범위를 벗어났습니다.") == NULL) {
        return 0;
    }

    return error.line == 1 && error.column == 27;
}

static int test_insert_missing_schema_column_fail(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_missing_schema_column_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (id, name) VALUES (1, 'kim');",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    if (strstr(error.message, "모든 컬럼 값이 필요합니다.") == NULL) {
        return 0;
    }

    return access(data_path, F_OK) != 0;
}

static int test_insert_auto_primary_key_success(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_auto_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (name, age) VALUES ('kim', 20);"
                        "INSERT INTO users (name, age) VALUES ('lee', 30);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!execute_program(&config, &program, &error)) {
        return 0;
    }

    return file_contains_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n");
}

static int test_insert_auto_primary_key_uses_existing_max(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_auto_pk_existing_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "id,name,age\n5,park,40\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (name, age) VALUES ('kim', 20);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!execute_program(&config, &program, &error)) {
        return 0;
    }

    return file_contains_text(data_path, "id,name,age\n5,park,40\n6,kim,20\n");
}

static int test_insert_explicit_primary_key_advances_auto_value(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_explicit_then_auto_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (id, name, age) VALUES (10, 'park', 40);"
                        "INSERT INTO users (name, age) VALUES ('kim', 20);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!execute_program(&config, &program, &error)) {
        return 0;
    }

    return file_contains_text(data_path, "id,name,age\n10,park,40\n11,kim,20\n");
}

static int test_insert_duplicate_primary_key_fail(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_duplicate_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                        "INSERT INTO users (id, name, age) VALUES (1, 'lee', 30);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    if (strstr(error.message, "PK 값이 이미 존재합니다.") == NULL) {
        return 0;
    }

    return file_contains_text(data_path, "id,name,age\n1,kim,20\n") &&
           !file_contains_text(data_path, "lee,30");
}

static int test_insert_duplicate_primary_key_existing_csv_fail(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_duplicate_existing_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "id,name,age\n7,park,40\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users (id, name, age) VALUES (7, 'kim', 20);",
                        &program,
                        &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    if (strstr(error.message, "PK 값이 이미 존재합니다.") == NULL) {
        return 0;
    }

    return file_contains_text(data_path, "id,name,age\n7,park,40\n") &&
           !file_contains_text(data_path, "kim,20");
}

static int test_select_where_index_and_scan_success(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char sql_path[256];
    char output_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_index_scan_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users (name, age) VALUES ('kim', 20);"
                         "INSERT INTO users (name, age) VALUES ('lee', 30);"
                         "SELECT * FROM users WHERE id = 2;"
                         "SELECT name FROM users WHERE name = 'kim';")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    return file_contains_text(output_path, "[INDEX] WHERE id = 2\n") &&
           file_contains_text(output_path, "id\tname\tage\n2\tlee\t30\n") &&
           file_contains_text(output_path, "[SCAN] WHERE name = kim\n") &&
           file_contains_text(output_path, "name\nkim\n");
}

static int test_select_where_comparison_operators(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char sql_path[256];
    char output_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_comparison_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users (name, age) VALUES ('kim', 20);"
                         "INSERT INTO users (name, age) VALUES ('lee', 30);"
                         "INSERT INTO users (name, age) VALUES ('park', 40);"
                         "SELECT * FROM users WHERE id > 2;"
                         "SELECT name FROM users WHERE id < 3;"
                         "SELECT id, name FROM users WHERE age != 20;")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    return file_contains_text(output_path, "[INDEX-RANGE] WHERE id > 2 (1 rows)\n") &&
           file_contains_text(output_path, "id\tname\tage\n3\tpark\t40\n") &&
           file_contains_text(output_path, "[INDEX-RANGE] WHERE id < 3 (2 rows)\n") &&
           file_contains_text(output_path, "name\nkim\nlee\n") &&
           file_contains_text(output_path, "[SCAN] WHERE age != 20\n") &&
           file_contains_text(output_path, "id\tname\n2\tlee\n3\tpark\n");
}

static int test_select_where_id_type_error(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_type_error_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("SELECT * FROM users WHERE id = 'kim';", &program, &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "WHERE 값 타입이 스키마와 맞지 않습니다.") != NULL;
}

static int test_insert_formula_like_string_fail(void)
{
    AppConfig config;
    SqlProgram program;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_formula_string_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!parse_sql_text("INSERT INTO users VALUES (1, '=2+3', 20);", &program, &error)) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (execute_program(&config, &program, &error)) {
        return 0;
    }

    if (strstr(error.message, "CSV에서 수식으로 해석될 수 없습니다.") == NULL) {
        return 0;
    }

    return error.line == 1 && error.column == 30;
}

static int test_select_allows_max_width_row(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char sql_path[256];
    char output_path[512];
    char long_value[SQLPROC_MAX_VALUE_LEN];
    FILE *schema_file;
    FILE *sql_file;
    int i;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_max_width_row_")) {
        return 0;
    }

    memset(long_value, 'a', SQLPROC_MAX_VALUE_LEN - 1);
    long_value[SQLPROC_MAX_VALUE_LEN - 1] = '\0';

    snprintf(schema_path, sizeof(schema_path), "%s/wide.schema", schema_dir);
    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    schema_file = fopen(schema_path, "wb");
    if (schema_file == NULL) {
        return 0;
    }

    for (i = 0; i < SQLPROC_MAX_COLUMNS; i++) {
        if (fprintf(schema_file,
                    "c%d:string%s",
                    i + 1,
                    i + 1 == SQLPROC_MAX_COLUMNS ? "\n" : ",") < 0) {
            fclose(schema_file);
            return 0;
        }
    }

    fclose(schema_file);

    sql_file = fopen(sql_path, "wb");
    if (sql_file == NULL) {
        return 0;
    }

    if (fputs("INSERT INTO wide VALUES (", sql_file) == EOF) {
        fclose(sql_file);
        return 0;
    }

    for (i = 0; i < SQLPROC_MAX_COLUMNS; i++) {
        if (fprintf(sql_file,
                    "'%s'%s",
                    long_value,
                    i + 1 == SQLPROC_MAX_COLUMNS ? "" : ", ") < 0) {
            fclose(sql_file);
            return 0;
        }
    }

    if (fputs(");\nSELECT * FROM wide;\n", sql_file) == EOF) {
        fclose(sql_file);
        return 0;
    }

    fclose(sql_file);

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    return file_contains_text(output_path, "c1\tc2\tc3") &&
           file_contains_text(output_path, long_value);
}

static int test_load_schema_duplicate_column_fail(void)
{
    TableSchema schema;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_schema_duplicate_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/weird.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int,id:int,name:string\n")) {
        return 0;
    }

    if (load_table_schema(schema_dir, "weird", &schema, &error)) {
        return 0;
    }

    return strstr(error.message, "스키마 컬럼 이름이 중복됩니다.") != NULL;
}

static int test_first_index_select_elapsed_includes_rebuild(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char data_path[512];
    char sql_path[256];
    char output_path[512];
    double first_ms;
    double second_ms;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_index_elapsed_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);
    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_generated_users_csv(data_path, 50000)) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "SELECT * FROM users WHERE id = 50000;\n"
                         "SELECT * FROM users WHERE id = 50000;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!read_first_two_elapsed_ms(output_path, &first_ms, &second_ms)) {
        return 0;
    }

    return first_ms > second_ms + 1.0;
}

static int test_load_schema_long_column_name_fail(void)
{
    TableSchema schema;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];
    char schema_line[160];
    char long_name[80];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_long_schema_name_")) {
        return 0;
    }

    memset(long_name, 'a', 70);
    long_name[70] = '\0';

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(schema_line, sizeof(schema_line), "%s:int,name:string\n", long_name);
    if (!write_text_file(schema_path, schema_line)) {
        return 0;
    }

    if (load_table_schema(schema_dir, "users", &schema, &error)) {
        return 0;
    }

    return strstr(error.message, "스키마 컬럼 이름이 너무 깁니다.") != NULL;
}

static int test_load_schema_detects_id_primary_key(void)
{
    TableSchema schema;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_schema_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "name:string,id:int,age:int\n")) {
        return 0;
    }

    if (!load_table_schema(schema_dir, "users", &schema, &error)) {
        return 0;
    }

    return schema.primary_key_index == 1;
}

static int test_load_schema_without_id_has_no_primary_key(void)
{
    TableSchema schema;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_schema_no_pk_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/logs.schema", schema_dir);
    if (!write_text_file(schema_path, "name:string,age:int\n")) {
        return 0;
    }

    if (!load_table_schema(schema_dir, "logs", &schema, &error)) {
        return 0;
    }

    return schema.primary_key_index == -1;
}

static int test_run_program_benchmark_mode(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char output_path[512];
    char benchmark_schema_path[512];
    char benchmark_csv_path[512];
    char benchmark_pk_sql_path[512];
    char benchmark_non_pk_sql_path[512];
    char original_users_csv_path[512];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_benchmark_mode_")) {
        return 0;
    }

    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(benchmark_schema_path,
             sizeof(benchmark_schema_path),
             "%s/benchmark/schemas/users.schema",
             data_dir);
    snprintf(benchmark_csv_path,
             sizeof(benchmark_csv_path),
             "%s/benchmark/data/users.csv",
             data_dir);
    snprintf(benchmark_pk_sql_path,
             sizeof(benchmark_pk_sql_path),
             "%s/benchmark/sql/pk_lookup.sql",
             data_dir);
    snprintf(benchmark_non_pk_sql_path,
             sizeof(benchmark_non_pk_sql_path),
             "%s/benchmark/sql/non_pk_lookup.sql",
             data_dir);
    snprintf(original_users_csv_path, sizeof(original_users_csv_path), "%s/users.csv", data_dir);

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    config.benchmark_mode = 1;

    if (!capture_run_program_with_input(&config,
                                        "abc\n0\n100\n.exit\n",
                                        output_path)) {
        return 0;
    }

    if (count_text_occurrences(output_path,
                               ">> 벤치마크를 위한 더미 데이터는 몇 개를 생성하시겠습니까? : ") != 3) {
        return 0;
    }

    if (count_text_occurrences(output_path, "양의 정수를 입력해 주세요.\n") != 2) {
        return 0;
    }

    if (!file_contains_text(output_path, "PK (id, cold)") ||
        !file_contains_text(output_path, "PK (id, warm)") ||
        !file_contains_text(output_path, "not PK (name)") ||
        !file_contains_text(output_path, "sqlproc interactive mode\n") ||
        !file_contains_text(output_path, "sqlproc> ")) {
        return 0;
    }

    if (!path_exists(benchmark_schema_path) ||
        !path_exists(benchmark_csv_path) ||
        !path_exists(benchmark_pk_sql_path) ||
        !path_exists(benchmark_non_pk_sql_path)) {
        return 0;
    }

    return !path_exists(original_users_csv_path);
}

int main(void)
{
    if (!test_parse_arguments_success()) {
        fprintf(stderr, "test_parse_arguments_success failed\n");
        return 1;
    }

    if (!test_parse_arguments_fail()) {
        fprintf(stderr, "test_parse_arguments_fail failed\n");
        return 1;
    }

    if (!test_parse_arguments_benchmark_short_success()) {
        fprintf(stderr, "test_parse_arguments_benchmark_short_success failed\n");
        return 1;
    }

    if (!test_parse_arguments_benchmark_long_success()) {
        fprintf(stderr, "test_parse_arguments_benchmark_long_success failed\n");
        return 1;
    }

    if (!test_parse_arguments_reject_mixed_modes()) {
        fprintf(stderr, "test_parse_arguments_reject_mixed_modes failed\n");
        return 1;
    }

    if (!test_tokenize_select()) {
        fprintf(stderr, "test_tokenize_select failed\n");
        return 1;
    }

    if (!test_tokenize_multiline_string_fail()) {
        fprintf(stderr, "test_tokenize_multiline_string_fail failed\n");
        return 1;
    }

    if (!test_parse_insert_statement()) {
        fprintf(stderr, "test_parse_insert_statement failed\n");
        return 1;
    }

    if (!test_parse_insert_without_column_list()) {
        fprintf(stderr, "test_parse_insert_without_column_list failed\n");
        return 1;
    }

    if (!test_parse_select_where_equals()) {
        fprintf(stderr, "test_parse_select_where_equals failed\n");
        return 1;
    }

    if (!test_parse_select_where_greater()) {
        fprintf(stderr, "test_parse_select_where_greater failed\n");
        return 1;
    }

    if (!test_bptree_single_key_search()) {
        fprintf(stderr, "test_bptree_single_key_search failed\n");
        return 1;
    }

    if (!test_bptree_multiple_keys_and_split()) {
        fprintf(stderr, "test_bptree_multiple_keys_and_split failed\n");
        return 1;
    }

    if (!test_bptree_duplicate_key_fail()) {
        fprintf(stderr, "test_bptree_duplicate_key_fail failed\n");
        return 1;
    }

    if (!test_bptree_thousand_keys()) {
        fprintf(stderr, "test_bptree_thousand_keys failed\n");
        return 1;
    }

    if (!test_run_program_success()) {
        fprintf(stderr, "test_run_program_success failed\n");
        return 1;
    }

    if (!test_parse_empty_sql_fail()) {
        fprintf(stderr, "test_parse_empty_sql_fail failed\n");
        return 1;
    }

    if (!test_insert_and_select_execution()) {
        fprintf(stderr, "test_insert_and_select_execution failed\n");
        return 1;
    }

    if (!test_storage_print_rows_without_data_file()) {
        fprintf(stderr, "test_storage_print_rows_without_data_file failed\n");
        return 1;
    }

    if (!test_storage_print_rows_header_mismatch()) {
        fprintf(stderr, "test_storage_print_rows_header_mismatch failed\n");
        return 1;
    }

    if (!test_insert_int_overflow_fail()) {
        fprintf(stderr, "test_insert_int_overflow_fail failed\n");
        return 1;
    }

    if (!test_insert_missing_schema_column_fail()) {
        fprintf(stderr, "test_insert_missing_schema_column_fail failed\n");
        return 1;
    }

    if (!test_insert_auto_primary_key_success()) {
        fprintf(stderr, "test_insert_auto_primary_key_success failed\n");
        return 1;
    }

    if (!test_insert_auto_primary_key_uses_existing_max()) {
        fprintf(stderr, "test_insert_auto_primary_key_uses_existing_max failed\n");
        return 1;
    }

    if (!test_insert_explicit_primary_key_advances_auto_value()) {
        fprintf(stderr, "test_insert_explicit_primary_key_advances_auto_value failed\n");
        return 1;
    }

    if (!test_insert_duplicate_primary_key_fail()) {
        fprintf(stderr, "test_insert_duplicate_primary_key_fail failed\n");
        return 1;
    }

    if (!test_insert_duplicate_primary_key_existing_csv_fail()) {
        fprintf(stderr, "test_insert_duplicate_primary_key_existing_csv_fail failed\n");
        return 1;
    }

    if (!test_select_where_index_and_scan_success()) {
        fprintf(stderr, "test_select_where_index_and_scan_success failed\n");
        return 1;
    }

    if (!test_select_where_comparison_operators()) {
        fprintf(stderr, "test_select_where_comparison_operators failed\n");
        return 1;
    }

    if (!test_select_where_id_type_error()) {
        fprintf(stderr, "test_select_where_id_type_error failed\n");
        return 1;
    }

    if (!test_insert_formula_like_string_fail()) {
        fprintf(stderr, "test_insert_formula_like_string_fail failed\n");
        return 1;
    }

    if (!test_select_allows_max_width_row()) {
        fprintf(stderr, "test_select_allows_max_width_row failed\n");
        return 1;
    }

    if (!test_first_index_select_elapsed_includes_rebuild()) {
        fprintf(stderr, "test_first_index_select_elapsed_includes_rebuild failed\n");
        return 1;
    }

    if (!test_load_schema_long_column_name_fail()) {
        fprintf(stderr, "test_load_schema_long_column_name_fail failed\n");
        return 1;
    }

    if (!test_load_schema_duplicate_column_fail()) {
        fprintf(stderr, "test_load_schema_duplicate_column_fail failed\n");
        return 1;
    }

    if (!test_load_schema_detects_id_primary_key()) {
        fprintf(stderr, "test_load_schema_detects_id_primary_key failed\n");
        return 1;
    }

    if (!test_load_schema_without_id_has_no_primary_key()) {
        fprintf(stderr, "test_load_schema_without_id_has_no_primary_key failed\n");
        return 1;
    }

    if (!test_run_program_benchmark_mode()) {
        fprintf(stderr, "test_run_program_benchmark_mode failed\n");
        return 1;
    }

    printf("All tests passed.\n");
    return 0;
}

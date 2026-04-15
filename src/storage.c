#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

/*
 * storage.c는 CSV 파일 저장과 조회를 담당하는 얇은 스토리지 계층입니다.
 * 실행기는 "무슨 데이터를 읽고 쓸지"만 결정하고,
 * 실제 파일 경로/헤더/행 입출력은 이 모듈에 맡깁니다.
 */

#define STORAGE_MAX_PATH_LEN 512
#define STORAGE_MAX_ROW_LEN SQLPROC_MAX_CSV_ROW_LEN

static void set_file_error(ErrorInfo *error, const char *message)
{
    /* 파일 시스템/CSV 형식 오류는 SQL 위치 없이 메시지만 기록합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static void build_table_path(char *dest,
                             size_t dest_size,
                             const char *base_dir,
                             const char *table_name,
                             const char *extension)
{
    /* data_dir/users.csv 같은 실제 파일 경로를 조립합니다. */
    snprintf(dest, dest_size, "%s/%s%s", base_dir, table_name, extension);
}

static int validate_line_length(const char *line,
                                size_t line_size,
                                FILE *file,
                                ErrorInfo *error,
                                const char *message)
{
    size_t line_len;

    /*
     * fgets가 버퍼 끝까지 읽었는데 개행을 못 만났다면
     * 아직 다음 조각이 남아 있다는 뜻이므로 "한 행이 너무 길다"고 봅니다.
     */
    line_len = strlen(line);
    if (line_len == line_size - 1 && line[line_len - 1] != '\n' && !feof(file)) {
        set_file_error(error, message);
        return 0;
    }

    return 1;
}

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count)
{
    int in_quotes;
    int row_index;
    int text_index;
    int i;

    /* CSV 한 줄을 컬럼 문자열 배열로 분해합니다. 큰따옴표 이스케이프도 처리합니다. */
    in_quotes = 0;
    row_index = 0;
    text_index = 0;

    for (i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r'; i++) {
        if (row_index >= SQLPROC_MAX_COLUMNS) {
            return 0;
        }

        if (line[i] == '"') {
            if (in_quotes && line[i + 1] != '\0' && line[i + 1] == '"') {
                if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
                    return 0;
                }

                values[row_index][text_index] = '"';
                text_index += 1;
                i += 1;
                continue;
            }

            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && line[i] == ',') {
            values[row_index][text_index] = '\0';
            row_index += 1;
            text_index = 0;
            continue;
        }

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
            return 0;
        }

        values[row_index][text_index] = line[i];
        text_index += 1;
    }

    if (row_index >= SQLPROC_MAX_COLUMNS) {
        return 0;
    }

    values[row_index][text_index] = '\0';
    *value_count = row_index + 1;
    return 1;
}

static int write_csv_field(FILE *file, const char *text)
{
    int needs_quote;
    const char *cursor;

    /*
     * CSV 필드 1개를 안전하게 저장합니다.
     * 쉼표/따옴표/개행이 있으면 큰따옴표로 감싸고 내부 따옴표는 이스케이프합니다.
     */
    needs_quote = 0;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == ',' || *cursor == '"' || *cursor == '\n') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        return fputs(text, file) != EOF;
    }

    if (fputc('"', file) == EOF) {
        return 0;
    }

    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            if (fputc('"', file) == EOF) {
                return 0;
            }
        }

        if (fputc(*cursor, file) == EOF) {
            return 0;
        }
    }

    return fputc('"', file) != EOF;
}

static int write_csv_row(FILE *file,
                         char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                         int value_count)
{
    int i;

    /* 컬럼 배열 1개를 CSV 한 줄로 저장합니다. */
    for (i = 0; i < value_count; i++) {
        if (i > 0) {
            if (fputc(',', file) == EOF) {
                return 0;
            }
        }

        if (!write_csv_field(file, values[i])) {
            return 0;
        }
    }

    return fputc('\n', file) != EOF;
}

static int ensure_append_starts_on_new_line(FILE *file)
{
    long file_size;
    int last_character;

    /*
     * 사용자가 CSV를 직접 편집해 마지막 행 끝의 개행을 지웠을 수 있습니다.
     * append 전 마지막 문자가 개행이 아니면 새 행이 이전 행에 붙지 않도록 보정합니다.
     */
    if (fseek(file, 0, SEEK_END) != 0) {
        return 0;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        return 0;
    }

    if (file_size == 0) {
        return 1;
    }

    if (fseek(file, -1, SEEK_END) != 0) {
        return 0;
    }

    last_character = fgetc(file);
    if (last_character == EOF) {
        return 0;
    }

    if (last_character != '\n') {
        if (fseek(file, 0, SEEK_END) != 0) {
            return 0;
        }

        return fputc('\n', file) != EOF;
    }

    return fseek(file, 0, SEEK_END) == 0;
}

static int validate_header_values(const TableSchema *schema,
                                  char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                  int header_count,
                                  ErrorInfo *error,
                                  const char *count_message,
                                  const char *order_message)
{
    int i;

    if (header_count != schema->column_count) {
        set_file_error(error, count_message);
        return 0;
    }

    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(header_values[i], schema->columns[i].name) != 0) {
            set_file_error(error, order_message);
            return 0;
        }
    }

    return 1;
}

static int ensure_data_file(const AppConfig *config,
                            const TableSchema *schema,
                            ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int header_count;
    FILE *file;
    int i;

    /*
     * 데이터 파일이 이미 있으면 헤더가 현재 스키마와 일치하는지 검증하고,
     * 없으면 새 CSV 파일을 만들고 헤더를 기록합니다.
     */
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file != NULL) {
        if (fgets(line, sizeof(line), file) == NULL) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더를 읽을 수 없습니다.");
            return 0;
        }

        if (!validate_line_length(line,
                                  sizeof(line),
                                  file,
                                  error,
                                  "기존 데이터 파일 헤더 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "기존 데이터 파일 헤더가 스키마와 다릅니다.",
                                    "기존 데이터 파일 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }

        fclose(file);
        return 1;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 만들 수 없습니다.");
        return 0;
    }

    for (i = 0; i < schema->column_count; i++) {
        if (i > 0) {
            if (fputc(',', file) == EOF) {
                fclose(file);
                set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
                return 0;
            }
        }

        if (fputs(schema->columns[i].name, file) == EOF) {
            fclose(file);
            set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
            return 0;
        }
    }

    if (fputc('\n', file) == EOF) {
        fclose(file);
        set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
        return 0;
    }

    fclose(file);
    return 1;
}

static void print_selected_header(const TableSchema *schema,
                                  const int selected_indices[SQLPROC_MAX_COLUMNS],
                                  int selected_count)
{
    int i;

    /* 출력 결과 첫 줄에 선택된 컬럼 이름들을 탭 구분으로 출력합니다. */
    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            fputc('\t', stdout);
        }

        fputs(schema->columns[selected_indices[i]].name, stdout);
    }

    fputc('\n', stdout);
}

int storage_append_row(const AppConfig *config,
                       const TableSchema *schema,
                       char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                       long *out_offset,
                       ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    FILE *file;
    long row_offset;

    /* INSERT용 한 행을 파일에 추가하기 전에 헤더 상태를 먼저 맞춥니다. */
    if (!ensure_data_file(config, schema, error)) {
        return 0;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "a+b");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!ensure_append_starts_on_new_line(file)) {
        fclose(file);
        set_file_error(error, "데이터 파일 끝을 확인할 수 없습니다.");
        return 0;
    }

    row_offset = ftell(file);
    if (row_offset < 0) {
        fclose(file);
        set_file_error(error, "데이터 행 위치를 확인할 수 없습니다.");
        return 0;
    }

    if (!write_csv_row(file, row_values, schema->column_count)) {
        fclose(file);
        set_file_error(error, "데이터 행을 파일에 쓸 수 없습니다.");
        return 0;
    }

    if (out_offset != NULL) {
        *out_offset = row_offset;
    }

    fclose(file);
    return 1;
}

int storage_print_rows(const AppConfig *config,
                       const TableSchema *schema,
                       const int selected_indices[SQLPROC_MAX_COLUMNS],
                       int selected_count,
                       ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    FILE *file;

    /*
     * SELECT는 파일이 없으면 헤더만 출력하고,
     * 파일이 있으면 헤더를 검증한 뒤 전체 행을 순차 출력합니다.
     */
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            print_selected_header(schema, selected_indices, selected_count);
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    if (!validate_line_length(line,
                              sizeof(line),
                              file,
                              error,
                              "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    print_selected_header(schema, selected_indices, selected_count);

    while (fgets(line, sizeof(line), file) != NULL) {
        int value_count;
        int i;

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));

        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        for (i = 0; i < selected_count; i++) {
            if (i > 0) {
                fputc('\t', stdout);
            }

            fputs(values[selected_indices[i]], stdout);
        }

        fputc('\n', stdout);
    }

    fclose(file);
    return 1;
}

int storage_print_row_at_offset(const AppConfig *config,
                                const TableSchema *schema,
                                long offset,
                                const int selected_indices[SQLPROC_MAX_COLUMNS],
                                int selected_count,
                                ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    FILE *file;
    int value_count;
    int i;

    /*
     * B+ Tree 검색 결과로 받은 CSV row offset으로 바로 이동해
     * 필요한 한 줄만 읽습니다. offset이 음수이면 결과가 없는 조회로 보고
     * 헤더만 출력합니다.
     */
    print_selected_header(schema, selected_indices, selected_count);
    if (offset < 0) {
        return 1;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_file_error(error, "CSV 헤더를 읽을 수 없습니다.");
        return 0;
    }

    if (!validate_line_length(line, sizeof(line), file, error, "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        set_file_error(error, "CSV 행 위치로 이동할 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_file_error(error, "CSV 행을 읽을 수 없습니다.");
        return 0;
    }

    if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    memset(values, 0, sizeof(values));
    if (!parse_csv_line(line, values, &value_count)) {
        fclose(file);
        set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    if (value_count != schema->column_count) {
        fclose(file);
        set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
        return 0;
    }

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            fputc('\t', stdout);
        }

        fputs(values[selected_indices[i]], stdout);
    }
    fputc('\n', stdout);

    fclose(file);
    return 1;
}

static int csv_value_matches_where(const TableSchema *schema,
                                   const char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   int where_column_index,
                                   WhereOperator where_operator,
                                   const LiteralValue *where_value,
                                   int *matches)
{
    char *end_ptr;
    long row_value;
    long target_value;

    *matches = 0;

    if (schema->columns[where_column_index].type == DATA_TYPE_STRING) {
        int compare_result;

        compare_result = strcmp(values[where_column_index], where_value->text);
        if (where_operator == WHERE_OP_EQUAL) {
            *matches = compare_result == 0;
            return 1;
        }

        if (where_operator == WHERE_OP_NOT_EQUAL) {
            *matches = compare_result != 0;
            return 1;
        }

        return 0;
    }

    if (where_value->type != LITERAL_INT) {
        return 0;
    }

    errno = 0;
    row_value = strtol(values[where_column_index], &end_ptr, 10);
    if (errno == ERANGE || *end_ptr != '\0' ||
        row_value < INT_MIN || row_value > INT_MAX) {
        return 0;
    }

    errno = 0;
    target_value = strtol(where_value->text, &end_ptr, 10);
    if (errno == ERANGE || *end_ptr != '\0' ||
        target_value < INT_MIN || target_value > INT_MAX) {
        return 0;
    }

    if (where_operator == WHERE_OP_EQUAL) {
        *matches = row_value == target_value;
        return 1;
    }

    if (where_operator == WHERE_OP_GREATER) {
        *matches = row_value > target_value;
        return 1;
    }

    if (where_operator == WHERE_OP_LESS) {
        *matches = row_value < target_value;
        return 1;
    }

    if (where_operator == WHERE_OP_NOT_EQUAL) {
        *matches = row_value != target_value;
        return 1;
    }

    return 0;
}

int storage_print_rows_where_equals(const AppConfig *config,
                                    const TableSchema *schema,
                                    const int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int selected_count,
                                    int where_column_index,
                                    WhereOperator where_operator,
                                    const LiteralValue *where_value,
                                    ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    FILE *file;

    /*
     * PK가 아닌 WHERE 조건은 별도 인덱스가 없으므로 CSV를 처음부터 끝까지
     * 선형 탐색합니다.
     */
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            print_selected_header(schema, selected_indices, selected_count);
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        print_selected_header(schema, selected_indices, selected_count);
        return 1;
    }

    if (!validate_line_length(line, sizeof(line), file, error, "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    print_selected_header(schema, selected_indices, selected_count);

    while (fgets(line, sizeof(line), file) != NULL) {
        char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int value_count;
        int matches;
        int i;

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));
        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        if (!csv_value_matches_where(schema,
                                     values,
                                     where_column_index,
                                     where_operator,
                                     where_value,
                                     &matches)) {
            fclose(file);
            set_file_error(error, "CSV 조건 값을 비교할 수 없습니다.");
            return 0;
        }

        if (!matches) {
            continue;
        }

        for (i = 0; i < selected_count; i++) {
            if (i > 0) {
                fputc('\t', stdout);
            }

            fputs(values[selected_indices[i]], stdout);
        }
        fputc('\n', stdout);
    }

    fclose(file);
    return 1;
}

static int print_row_from_open_file(FILE *file,
                                    const TableSchema *schema,
                                    long offset,
                                    const int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int selected_count,
                                    ErrorInfo *error)
{
    char line[STORAGE_MAX_ROW_LEN];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int value_count;
    int i;

    if (fseek(file, offset, SEEK_SET) != 0) {
        set_file_error(error, "CSV 행 위치로 이동할 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "CSV 행을 읽을 수 없습니다.");
        return 0;
    }

    if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
        return 0;
    }

    memset(values, 0, sizeof(values));
    if (!parse_csv_line(line, values, &value_count)) {
        set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    if (value_count != schema->column_count) {
        set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
        return 0;
    }

    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            fputc('\t', stdout);
        }

        fputs(values[selected_indices[i]], stdout);
    }
    fputc('\n', stdout);

    return 1;
}

int storage_print_rows_at_offsets(const AppConfig *config,
                                  const TableSchema *schema,
                                  const long offsets[],
                                  int offset_count,
                                  const int selected_indices[SQLPROC_MAX_COLUMNS],
                                  int selected_count,
                                  ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    FILE *file;
    int i;

    print_selected_header(schema, selected_indices, selected_count);
    if (offset_count == 0) {
        return 1;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_file_error(error, "CSV 헤더를 읽을 수 없습니다.");
        return 0;
    }

    if (!validate_line_length(line, sizeof(line), file, error, "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    for (i = 0; i < offset_count; i++) {
        if (!print_row_from_open_file(file,
                                      schema,
                                      offsets[i],
                                      selected_indices,
                                      selected_count,
                                      error)) {
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 1;
}

int storage_find_max_int_value(const AppConfig *config,
                               const TableSchema *schema,
                               int column_index,
                               int *max_value,
                               ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    FILE *file;
    int found_value;

    /*
     * 자동 PK 발급용으로 CSV를 한 번 스캔해 특정 int 컬럼의 최댓값을 찾습니다.
     * 데이터 파일이 아직 없으면 max는 0으로 간주합니다.
     */
    *max_value = 0;
    found_value = 0;

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    if (!validate_line_length(line,
                              sizeof(line),
                              file,
                              error,
                              "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        char *end_ptr;
        long parsed_value;
        int value_count;

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));
        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        errno = 0;
        parsed_value = strtol(values[column_index], &end_ptr, 10);
        if (errno == ERANGE || *end_ptr != '\0' ||
            parsed_value < INT_MIN || parsed_value > INT_MAX) {
            fclose(file);
            set_file_error(error, "CSV 정수 값을 읽을 수 없습니다.");
            return 0;
        }

        if (!found_value || parsed_value > *max_value) {
            *max_value = (int)parsed_value;
            found_value = 1;
        }
    }

    fclose(file);
    return 1;
}

int storage_int_value_exists(const AppConfig *config,
                             const TableSchema *schema,
                             int column_index,
                             int target_value,
                             int *exists,
                             ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    FILE *file;

    /*
     * PK 중복 검사용 선형 탐색입니다.
     * B+ Tree가 들어오기 전까지는 CSV를 직접 읽어 같은 id가 있는지 확인합니다.
     */
    *exists = 0;

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    if (!validate_line_length(line,
                              sizeof(line),
                              file,
                              error,
                              "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        char *end_ptr;
        long parsed_value;
        int value_count;

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));
        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        errno = 0;
        parsed_value = strtol(values[column_index], &end_ptr, 10);
        if (errno == ERANGE || *end_ptr != '\0' ||
            parsed_value < INT_MIN || parsed_value > INT_MAX) {
            fclose(file);
            set_file_error(error, "CSV 정수 값을 읽을 수 없습니다.");
            return 0;
        }

        if ((int)parsed_value == target_value) {
            *exists = 1;
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 1;
}

int storage_rebuild_pk_index(const AppConfig *config,
                             const TableSchema *schema,
                             BPlusTree *index,
                             int *max_value,
                             ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    FILE *file;
    int found_value;

    /*
     * 메모리 인덱스는 프로그램을 끄면 사라집니다. 테이블을 처음 사용할 때
     * CSV를 다시 훑으며 id -> row offset 쌍을 B+ Tree에 재등록합니다.
     */
    *max_value = 0;
    found_value = 0;

    if (schema->primary_key_index < 0) {
        return 1;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    if (!validate_line_length(line,
                              sizeof(line),
                              file,
                              error,
                              "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    while (1) {
        char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        char *end_ptr;
        long parsed_value;
        long row_offset;
        long existing_offset;
        int value_count;

        row_offset = ftell(file);
        if (row_offset < 0) {
            fclose(file);
            set_file_error(error, "CSV 행 위치를 확인할 수 없습니다.");
            return 0;
        }

        if (fgets(line, sizeof(line), file) == NULL) {
            break;
        }

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));
        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        errno = 0;
        parsed_value = strtol(values[schema->primary_key_index], &end_ptr, 10);
        if (errno == ERANGE || *end_ptr != '\0' ||
            parsed_value < INT_MIN || parsed_value > INT_MAX) {
            fclose(file);
            set_file_error(error, "CSV 정수 값을 읽을 수 없습니다.");
            return 0;
        }

        if (bptree_search(index, (int)parsed_value, &existing_offset)) {
            (void)existing_offset;
            fclose(file);
            set_file_error(error, "CSV에 중복 PK 값이 있어 인덱스를 만들 수 없습니다.");
            return 0;
        }

        if (!bptree_insert(index, (int)parsed_value, row_offset)) {
            fclose(file);
            set_file_error(error, "PK 인덱스를 만들 수 없습니다.");
            return 0;
        }

        if (!found_value || parsed_value > *max_value) {
            *max_value = (int)parsed_value;
            found_value = 1;
        }
    }

    fclose(file);
    return 1;
}

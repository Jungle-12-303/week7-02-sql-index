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
 * 이 파일에서 row offset은 "몇 번째 행"이 아니라 "파일 안에서 그 행이 시작하는 바이트 위치"입니다.
 */

#define STORAGE_MAX_PATH_LEN 512
#define STORAGE_MAX_ROW_LEN SQLPROC_MAX_CSV_ROW_LEN

/* 파일 입출력 계층 오류 메시지를 ErrorInfo에 기록한다.
 *
 * 입력:
 * - error: 오류 정보를 저장할 구조체
 * - message: 사용자에게 보여 줄 오류 메시지
 * 출력:
 * - 반환값 없음
 * - error: 파일 단위 오류 메시지로 갱신됨
 */
static void set_file_error(ErrorInfo *error, const char *message)
{
    /* 파일 시스템/CSV 형식 오류는 SQL 위치 없이 메시지만 기록합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

/* base_dir, table_name, 확장자를 합쳐 실제 CSV 경로를 만든다.
 *
 * 입력:
 * - dest: 경로 문자열을 저장할 버퍼
 * - dest_size: dest의 크기
 * - base_dir: data 디렉터리 경로
 * - table_name: 테이블 이름
 * - extension: ".csv" 같은 파일 확장자
 * 출력:
 * - 반환값 없음
 * - dest: 완성된 파일 경로가 저장됨
 */
static void build_table_path(char *dest,
                             size_t dest_size,
                             const char *base_dir,
                             const char *table_name,
                             const char *extension)
{
    /* data_dir/users.csv 같은 실제 파일 경로를 조립합니다. */
    snprintf(dest, dest_size, "%s/%s%s", base_dir, table_name, extension);
}

/* fgets로 읽은 한 줄이 버퍼를 넘치지 않았는지 확인한다.
 *
 * 입력:
 * - line: 방금 읽은 줄 문자열
 * - line_size: line 버퍼 크기
 * - file: 읽기 중인 파일 포인터
 * - error: 실패 시 오류를 기록할 구조체
 * - message: 줄이 너무 길 때 사용할 오류 메시지
 * 출력:
 * - 반환값: 정상 길이면 1, 잘린 줄이면 0
 * - error: 실패 시 메시지가 기록됨
 */
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

/* CSV 한 줄을 컬럼 값 배열로 분해한다.
 *
 * 입력:
 * - line: 파싱할 CSV 한 줄
 * - values: 파싱 결과를 저장할 2차원 문자열 배열
 * - value_count: 실제 읽은 컬럼 수를 저장할 포인터
 * 출력:
 * - 반환값: 파싱 성공 시 1, 형식 또는 길이 오류 시 0
 * - values/value_count: 성공 시 각 컬럼 문자열과 개수가 채워짐
 */
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
            /*
             * CSV 안의 "" 는 실제 큰따옴표 1개를 뜻합니다.
             * in_quotes 상태에서 연속 따옴표를 만나면 값에 '"' 하나를 넣고
             * 입력 포인터를 한 칸 더 넘깁니다.
             */
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

/* 문자열 1개를 CSV 규칙에 맞게 이스케이프하여 파일에 쓴다.
 *
 * 입력:
 * - file: 출력 대상 파일 포인터
 * - text: 저장할 필드 문자열
 * 출력:
 * - 반환값: 쓰기 성공 시 1, 파일 쓰기 실패 시 0
 * - 부가 효과: file에 CSV 안전 형식으로 기록됨
 */
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

/* 컬럼 값 배열 한 줄을 CSV 행으로 출력한다.
 *
 * 입력:
 * - file: 출력 대상 파일 포인터
 * - values: 저장할 컬럼 문자열 배열
 * - value_count: 저장할 컬럼 개수
 * 출력:
 * - 반환값: 쓰기 성공 시 1, 실패 시 0
 * - 부가 효과: file에 CSV 한 줄이 추가됨
 */
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

/* append 전에 파일 끝이 개행으로 끝나는지 보장한다.
 *
 * 입력:
 * - file: append 모드로 열린 파일 포인터
 * 출력:
 * - 반환값: 준비 성공 시 1, fseek/쓰기 실패 시 0
 * - 부가 효과: 필요하면 파일 끝에 '\n'이 추가됨
 */
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

/* CSV 헤더 컬럼 수와 순서가 스키마와 일치하는지 확인한다.
 *
 * 입력:
 * - schema: 기준이 되는 테이블 스키마
 * - header_values: CSV 헤더를 파싱한 컬럼 이름 배열
 * - header_count: 헤더 컬럼 개수
 * - error: 실패 시 오류를 기록할 구조체
 * - count_message: 컬럼 수 불일치 시 메시지
 * - order_message: 컬럼 순서 불일치 시 메시지
 * 출력:
 * - 반환값: 헤더가 일치하면 1, 아니면 0
 * - error: 실패 시 원인 메시지가 기록됨
 */
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

/* 테이블 CSV 파일이 존재하고 스키마 헤더와 맞는지 보장한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 준비 성공 시 1, 파일/헤더 오류 시 0
 * - 부가 효과: 파일이 없으면 새 CSV를 만들고 헤더를 기록함
 */
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

/* SELECT 결과 첫 줄에 선택된 컬럼 헤더를 출력한다.
 *
 * 입력:
 * - schema: 대상 테이블 스키마
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * 출력:
 * - 반환값 없음
 * - 표준 출력(stdout)에 탭 구분 헤더가 출력됨
 */
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

/* CSV 파일 끝에 한 행을 추가하고 그 시작 오프셋을 돌려준다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - row_values: 스키마 순서로 정리된 행 값 배열
 * - out_offset: 기록된 행의 시작 오프셋을 저장할 포인터, NULL 허용
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: append 성공 시 1, 실패 시 0
 * - out_offset: 성공 시 새 행 시작 파일 위치가 저장됨
 */
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

/* WHERE 없는 SELECT를 위해 CSV 전체 행을 처음부터 끝까지 출력한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 출력 성공 시 1, 파일/CSV 형식 오류 시 0
 * - 부가 효과: 표준 출력(stdout)에 헤더와 데이터 행이 출력됨
 */
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

/* 특정 row offset으로 바로 이동해 한 행만 읽고 출력한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - offset: 읽을 행의 시작 파일 위치, 음수면 결과 없음으로 처리
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 출력 성공 시 1, 파일/CSV 형식 오류 시 0
 * - 부가 효과: 표준 출력(stdout)에 헤더와 필요 시 한 행이 출력됨
 */
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

/* CSV 한 행 값이 WHERE 조건과 일치하는지 비교한다.
 *
 * 입력:
 * - schema: 대상 테이블 스키마
 * - values: 현재 행의 컬럼 문자열 배열
 * - where_column_index: 비교할 컬럼 인덱스
 * - where_operator: 비교 연산자
 * - where_value: SQL WHERE 리터럴 값
 * - matches: 비교 결과를 저장할 포인터
 * 출력:
 * - 반환값: 비교 자체가 가능하면 1, 형식 오류면 0
 * - matches: 성공 시 조건 만족 여부가 1 또는 0으로 저장됨
 */
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

/* WHERE 조건이 있는 SELECT를 위해 CSV를 선형 탐색하며 일치 행만 출력한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * - where_column_index: 비교할 컬럼 인덱스
 * - where_operator: 비교 연산자
 * - where_value: 비교 기준 리터럴
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 출력 성공 시 1, 파일/CSV 형식 오류 시 0
 * - 부가 효과: 표준 출력(stdout)에 헤더와 조건 일치 행이 출력됨
 */
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

/* 이미 열린 CSV 파일에서 offset 위치의 한 행을 읽어 출력한다.
 *
 * 입력:
 * - file: 헤더를 이미 읽은 상태의 열린 파일 포인터
 * - schema: 대상 테이블 스키마
 * - offset: 읽을 행의 시작 파일 위치
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 출력 성공 시 1, 파일/CSV 형식 오류 시 0
 * - 부가 효과: 표준 출력(stdout)에 선택 컬럼 값이 한 줄 출력됨
 */
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

/* 여러 row offset 목록을 따라 필요한 행들만 순서대로 출력한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - offsets: 읽을 행 시작 위치 배열
 * - offset_count: offsets 개수
 * - selected_indices: 출력할 컬럼 인덱스 배열
 * - selected_count: 출력할 컬럼 개수
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 출력 성공 시 1, 파일/CSV 형식 오류 시 0
 * - 부가 효과: 표준 출력(stdout)에 헤더와 여러 행이 출력됨
 */
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

/* 특정 int 컬럼을 스캔해 최댓값을 찾는다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - column_index: 최댓값을 찾을 int 컬럼 인덱스
 * - max_value: 결과를 저장할 포인터
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 탐색 성공 시 1, 파일/CSV 형식 오류 시 0
 * - max_value: 성공 시 찾은 최댓값, 파일이 없거나 비어 있으면 0
 */
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

/* 특정 int 컬럼에 target 값이 이미 존재하는지 선형 탐색으로 확인한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - column_index: 검사할 int 컬럼 인덱스
 * - target_value: 찾을 값
 * - exists: 존재 여부를 저장할 포인터
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 탐색 성공 시 1, 파일/CSV 형식 오류 시 0
 * - exists: 성공 시 값 존재 여부가 1 또는 0으로 저장됨
 */
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

/* CSV 전체를 다시 읽어 메모리 기반 PK 인덱스를 재구성한다.
 *
 * 입력:
 * - config: data 디렉터리 정보
 * - schema: 대상 테이블 스키마
 * - index: 다시 채울 B+ Tree 인덱스
 * - max_value: 읽은 PK 최댓값을 저장할 포인터
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 재구성 성공 시 1, 파일/CSV/중복 PK 오류 시 0
 * - index: 성공 시 모든 PK -> row offset 쌍이 삽입됨
 * - max_value: 성공 시 가장 큰 PK 값이 저장됨
 */
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

        /*
         * ftell은 "다음 fgets가 읽을 행의 시작 위치"를 알려 줍니다.
         * 그래서 먼저 현재 위치를 저장한 뒤 그 다음 줄을 읽어야
         * PK -> CSV row offset 매핑이 정확해집니다.
         */
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

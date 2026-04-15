#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/*
 * schema.c는 <table>.schema 파일을 읽어 TableSchema 구조체로 바꾸는 모듈입니다.
 * 현재 형식 예:
 *   id:int,name:string,age:int
 */

/* 스키마 로딩 단계의 오류 메시지를 ErrorInfo에 기록한다.
 *
 * 입력:
 * - error: 오류 정보를 저장할 구조체
 * - message: 사용자에게 보여 줄 오류 문자열
 * 출력:
 * - 반환값 없음
 * - error: 파일 단위 오류 메시지로 갱신됨
 */
static void set_error(ErrorInfo *error, const char *message)
{
    /* 스키마 로더 오류는 파일 단위 오류라 줄/열 없이 메시지만 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

/* 문자열을 소문자로 변환해 별도 버퍼에 복사한다.
 *
 * 입력:
 * - dest: 결과를 저장할 버퍼
 * - dest_size: dest의 크기
 * - src: 원본 문자열
 * 출력:
 * - 반환값 없음
 * - dest: 소문자로 정규화된 문자열이 저장됨
 */
static void to_lowercase_copy(char *dest, size_t dest_size, const char *src)
{
    size_t i;

    /* 컬럼 이름, 타입을 대소문자 영향 없이 다루기 위한 소문자 복사입니다. */
    for (i = 0; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = (char)tolower((unsigned char)src[i]);
    }

    dest[i] = '\0';
}

/* 스키마 파일의 타입 문자열을 내부 enum 값으로 바꾼다.
 *
 * 입력:
 * - text: "int", "string" 같은 타입 문자열
 * - data_type: 변환 결과를 저장할 포인터
 * 출력:
 * - 반환값: 지원 타입이면 1, 알 수 없는 타입이면 0
 * - data_type: 성공 시 대응하는 DataType 값이 저장됨
 */
static int parse_data_type(const char *text, DataType *data_type)
{
    /* 스키마 파일의 타입 문자열을 내부 DataType enum으로 바꿉니다. */
    if (strcmp(text, "int") == 0) {
        *data_type = DATA_TYPE_INT;
        return 1;
    }

    if (strcmp(text, "string") == 0) {
        *data_type = DATA_TYPE_STRING;
        return 1;
    }

    return 0;
}

/* 이미 읽은 스키마 컬럼 이름과 중복되는 이름이 있는지 확인한다.
 *
 * 입력:
 * - schema: 현재까지 채워진 테이블 스키마
 * - name: 새로 검사할 컬럼 이름
 * 출력:
 * - 반환값: 중복이면 1, 아니면 0
 */
static int has_duplicate_column_name(const TableSchema *schema, const char *name)
{
    int i;

    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return 1;
        }
    }

    return 0;
}

/* <table>.schema 파일을 읽어 TableSchema 구조체를 완성한다.
 *
 * 입력:
 * - schema_dir: 스키마 파일들이 들어 있는 디렉터리
 * - table_name: 읽을 테이블 이름
 * - schema: 결과를 저장할 스키마 구조체
 * - error: 실패 시 오류 메시지를 기록할 구조체
 * 출력:
 * - 반환값: 로딩 성공 시 1, 형식 오류 또는 파일 오류 시 0
 * - schema: 성공 시 컬럼 이름, 타입, PK 위치 정보가 채워짐
 *   이 프로젝트에서는 이름이 정확히 `id`이고 타입이 `int`인 컬럼을
 *   primary_key_index로 자동 인식함
 * - error: 실패 시 원인이 기록됨
 */
int load_table_schema(const char *schema_dir,
                      const char *table_name,
                      TableSchema *schema,
                      ErrorInfo *error)
{
    char path[512];
    char line[SQLPROC_MAX_SCHEMA_LINE_LEN];
    char *entry;
    char *cursor;
    FILE *file;

    /*
     * 실행기 모듈이 사용할 TableSchema를 채웁니다.
     * - table_name
     * - 컬럼 개수와 순서
     * - 각 컬럼 타입
     */
    memset(schema, 0, sizeof(*schema));
    memset(error, 0, sizeof(*error));
    schema->primary_key_index = -1;
    snprintf(schema->table_name, sizeof(schema->table_name), "%s", table_name);
    snprintf(path, sizeof(path), "%s/%s.schema", schema_dir, table_name);

    /* 테이블 이름에 대응하는 스키마 파일 1개를 읽습니다. */
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, "스키마 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        set_error(error, "스키마 파일이 비어 있습니다.");
        return 0;
    }

    fclose(file);

    line[strcspn(line, "\r\n")] = '\0';
    cursor = line;

    /*
     * 한 줄 스키마를 쉼표와 콜론 기준으로 제자리에서 잘라 가며 읽습니다.
     * 예: id:int,name:string -> [id:int] [name:string]
     */
    while (*cursor != '\0') {
        char *colon;
        char lower_name[SQLPROC_MAX_NAME_LEN];
        char lower_type[SQLPROC_MAX_NAME_LEN];

        if (schema->column_count >= SQLPROC_MAX_COLUMNS) {
            set_error(error, "스키마 컬럼 수가 최대 개수를 넘었습니다.");
            return 0;
        }

        entry = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor += 1;
        }

        if (*cursor == ',') {
            *cursor = '\0';
            cursor += 1;
        }

        colon = strchr(entry, ':');
        if (colon == NULL) {
            set_error(error, "스키마 형식이 잘못되었습니다.");
            return 0;
        }

        *colon = '\0';

        if (strlen(entry) >= SQLPROC_MAX_NAME_LEN) {
            set_error(error, "스키마 컬럼 이름이 너무 깁니다.");
            return 0;
        }

        to_lowercase_copy(lower_name, sizeof(lower_name), entry);
        to_lowercase_copy(lower_type, sizeof(lower_type), colon + 1);

        if (lower_name[0] == '\0') {
            set_error(error, "스키마 컬럼 이름이 비어 있습니다.");
            return 0;
        }

        if (has_duplicate_column_name(schema, lower_name)) {
            set_error(error, "스키마 컬럼 이름이 중복됩니다.");
            return 0;
        }

        if (!parse_data_type(lower_type, &schema->columns[schema->column_count].type)) {
            set_error(error, "지원하지 않는 스키마 타입입니다.");
            return 0;
        }

        snprintf(schema->columns[schema->column_count].name,
                 sizeof(schema->columns[schema->column_count].name),
                 "%s",
                 lower_name);

        /* 별도 PRIMARY KEY 문법 대신 `id:int` 컬럼을 PK로 간주합니다. */
        if (strcmp(lower_name, "id") == 0 &&
            schema->columns[schema->column_count].type == DATA_TYPE_INT) {
            schema->primary_key_index = schema->column_count;
        }

        schema->column_count += 1;
    }

    if (schema->column_count == 0) {
        set_error(error, "스키마에 컬럼이 없습니다.");
        return 0;
    }

    return 1;
}

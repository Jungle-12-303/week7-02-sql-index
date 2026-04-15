#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/*
 * 이 파일은 프로그램의 "실행 제어"를 담당합니다.
 * - 명령행 인자를 읽어 AppConfig를 채우고
 * - SQL 파일을 읽어 토크나이저 -> 파서 -> 실행기로 넘기고
 * - 사용자에게 오류를 출력합니다.
 *
 * 즉 main.c와 실제 SQL 엔진 사이를 연결하는 중간 계층입니다.
 */

static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error);

/* 명령행 인자를 읽어 실행 설정(AppConfig) 구조체를 채운다.
 *
 * 입력:
 * - argc: 커맨드라인 인자 개수
 * - argv: 커맨드라인 인자 문자열 배열
 * - config: 해석 결과를 저장할 설정 구조체
 * 출력:
 * - 반환값: 지원 형식이면 1, 잘못된 인자 구성이면 0
 * - config: 성공 시 schema_dir, data_dir, input_path 또는 interactive_mode가 채워짐
 */
int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;

    /*
     * 지원하는 실행 형식:
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> <input.sql>
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> --interactive
     *   -> argc == 6
     */
    if (argc != 6) {
        return 0;
    }

    /*
     * 이전 실행 값이 남지 않도록 config 전체를 0으로 초기화합니다.
     */
    memset(config, 0, sizeof(*config));

    /*
     * argv는 "--옵션 값" 쌍으로 들어오므로 2칸씩 전진합니다.
     * 마지막 인자(argv[5])는 SQL 파일 경로입니다.
     */
    for (i = 1; i < argc - 1; i += 2) {
        if (strcmp(argv[i], "--schema-dir") == 0) {
            snprintf(config->schema_dir, sizeof(config->schema_dir), "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            snprintf(config->data_dir, sizeof(config->data_dir), "%s", argv[i + 1]);
        } else {
            return 0;
        }
    }

    /*
     * schema_dir, data_dir는 항상 필요합니다.
     */
    if (config->schema_dir[0] == '\0' || config->data_dir[0] == '\0') {
        return 0;
    }

    if (strcmp(argv[argc - 1], "--interactive") == 0) {
        config->interactive_mode = 1;
    } else {
        snprintf(config->input_path, sizeof(config->input_path), "%s", argv[argc - 1]);
    }

    return 1;
}

/* SQL 파일 전체를 읽어 실행용 문자열 버퍼에 적재한다.
 *
 * 입력:
 * - path: 읽을 SQL 파일 경로
 * - buffer: 파일 내용을 저장할 문자 버퍼
 * - buffer_size: buffer의 전체 크기
 * - error: 실패 원인을 기록할 오류 구조체
 * 출력:
 * - 반환값: 읽기 성공 시 1, 파일 열기/읽기 실패 시 0
 * - buffer: 성공 시 널 종료된 SQL 문자열이 저장됨
 * - error: 실패 시 메시지가 채워짐
 */
int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error)
{
    FILE *file;
    size_t total_size;

    /*
     * 호출자에게 이전 오류 정보가 섞이지 않도록 먼저 초기화합니다.
     */
    memset(error, 0, sizeof(*error));

    /*
     * SQL 파일을 바이너리 모드로 열어 전체 내용을 그대로 읽습니다.
     * 이 프로젝트는 파일 내용을 메모리에 한 번 올린 뒤 문자열처럼 처리합니다.
     */
    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error->message, sizeof(error->message), "SQL 파일을 열 수 없습니다.");
        return 0;
    }

    /*
     * 버퍼 끝 1바이트는 문자열 종료 문자('\0')를 위해 비워 둡니다.
     */
    total_size = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    if (total_size == buffer_size - 1) {
        char probe;
        size_t extra_size;

        /*
         * 버퍼가 가득 찼을 때만 1바이트를 더 읽어 실제로 더 큰 파일인지 확인합니다.
         * 한 번 더 읽혔다면 잘린 내용을 실행하지 않고 실패합니다.
         */
        extra_size = fread(&probe, 1, 1, file);
        if (extra_size > 0) {
            fclose(file);
            snprintf(error->message, sizeof(error->message), "SQL 파일이 너무 큽니다.");
            return 0;
        }
    }

    fclose(file);
    buffer[total_size] = '\0';
    return 1;
}

/* ErrorInfo에 담긴 내용을 사용자에게 보기 좋은 형식으로 출력한다.
 *
 * 입력:
 * - error: 메시지와 위치 정보를 담은 오류 구조체
 * 출력:
 * - 반환값 없음
 * - 표준 오류(stderr)에 오류 메시지와 필요 시 line/column 정보 출력
 */
void print_error(const ErrorInfo *error)
{
    /*
     * 빈 오류는 출력하지 않습니다.
     * 호출자가 "오류가 없다"는 상태를 빈 message로 표현할 수 있기 때문입니다.
     */
    if (error->message[0] == '\0') {
        return;
    }

    /*
     * line/column이 있으면 파서/실행기에서 위치를 계산해 넣은 경우이므로
     * 사용자에게 함께 보여 줍니다.
     */
    if (error->line > 0) {
        fprintf(stderr, "오류: %s (line %d, column %d)\n",
                error->message,
                error->line,
                error->column);
        return;
    }

    fprintf(stderr, "오류: %s\n", error->message);
}

/* SQL 문자열 하나를 토큰화, 파싱, 실행까지 한 번에 처리한다.
 *
 * 입력:
 * - config: 스키마/데이터 경로 등 실행 설정
 * - sql_text: 실행할 SQL 문자열
 * - error: 실패 시 오류를 기록할 구조체
 * 출력:
 * - 반환값: 실행 성공 시 1, 중간 단계 실패 시 0
 * - error: 실패 단계의 메시지와 위치 정보가 채워질 수 있음
 */
static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error)
{
    TokenList tokens;
    SqlProgram program;

    /*
     * 이 함수는 "SQL 문자열 1개를 실제 실행하는 공통 파이프라인"입니다.
     *
     * 흐름:
     * 1. SQL 문자열 -> TokenList
     * 2. TokenList -> SqlProgram
     * 3. SqlProgram -> execute_program
     */
    if (!tokenize_sql(sql_text, &tokens, error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, error)) {
        return 0;
    }

    return execute_program(config, &program, error);
}

/* 대화형 입력 한 줄이 종료 명령인지 판별한다.
 *
 * 입력:
 * - line: 사용자가 입력한 한 줄 문자열
 * 출력:
 * - 반환값: 종료 명령이면 1, 아니면 0
 */
static int is_exit_command(const char *line)
{
    return strcmp(line, ".exit") == 0 ||
           strcmp(line, "exit") == 0 ||
           strcmp(line, "quit") == 0;
}

/* fgets로 읽은 줄 끝의 개행 문자를 제거한다.
 *
 * 입력:
 * - line: 줄바꿈이 포함될 수 있는 입력 문자열 버퍼
 * 출력:
 * - 반환값 없음
 * - line: 끝의 '\n', '\r'가 제거된 상태로 수정됨
 */
static void trim_line_end(char *line)
{
    size_t length;

    length = strlen(line);
    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[length - 1] = '\0';
        length--;
    }
}

/* interactive 모드에서 한 줄씩 SQL을 읽고 즉시 실행한다.
 *
 * 입력:
 * - config: 스키마/데이터 경로를 포함한 실행 설정
 * 출력:
 * - 반환값: 사용자가 종료했거나 EOF를 만나면 0
 * - 부가 효과: 프롬프트와 실행 결과를 표준 출력에 표시
 */
static int run_interactive_program(const AppConfig *config)
{
    char line[SQLPROC_MAX_SQL_SIZE];

    printf("sqlproc interactive mode\n");
    printf("type .exit to quit\n");

    while (1) {
        ErrorInfo error;

        printf("sqlproc> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            return 0;
        }

        trim_line_end(line);

        if (is_exit_command(line)) {
            return 0;
        }

        if (line[0] == '\0') {
            continue;
        }

        if (!run_sql_text(config, line, &error)) {
            print_error(&error);
        }
    }
}

/* 설정에 따라 파일 실행 모드 또는 interactive 모드를 시작한다.
 *
 * 입력:
 * - config: parse_arguments가 채운 실행 설정
 * 출력:
 * - 반환값: 성공 시 0, 오류 발생 시 1
 * - 부가 효과: SQL 파일 실행 또는 대화형 루프 수행
 */
int run_program(const AppConfig *config)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];
    ErrorInfo error;

    if (config->interactive_mode) {
        return run_interactive_program(config);
    }

    /*
     * SQL 파일을 읽어 한 번 실행합니다.
     */
    if (!load_sql_file(config->input_path, sql_text, sizeof(sql_text), &error)) {
        print_error(&error);
        return 1;
    }

    if (!run_sql_text(config, sql_text, &error)) {
        print_error(&error);
        return 1;
    }

    return 0;
}

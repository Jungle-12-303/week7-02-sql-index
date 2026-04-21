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

int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;
    int has_input_path;
    int mode_count;

    /*
     * 이전 실행 값이 남지 않도록 config 전체를 0으로 초기화합니다.
     */
    memset(config, 0, sizeof(*config));
    has_input_path = 0;
    mode_count = 0;

    /*
     * 지원하는 실행 형식:
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> <input.sql>
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> --interactive
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> --benchmark
     *
     * 옵션 순서가 조금 달라도 읽을 수 있도록 argv 전체를 왼쪽부터 검사합니다.
     */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--schema-dir") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }

            snprintf(config->schema_dir, sizeof(config->schema_dir), "%s", argv[i + 1]);
            i += 1;
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }

            snprintf(config->data_dir, sizeof(config->data_dir), "%s", argv[i + 1]);
            i += 1;
        } else if (strcmp(argv[i], "--interactive") == 0) {
            config->interactive_mode = 1;
            mode_count += 1;
        } else if (strcmp(argv[i], "-b") == 0 ||
                   strcmp(argv[i], "--benchmark") == 0) {
            config->benchmark_mode = 1;
            mode_count += 1;
        } else if (argv[i][0] == '-') {
            return 0;
        } else {
            if (has_input_path) {
                return 0;
            }

            snprintf(config->input_path, sizeof(config->input_path), "%s", argv[i]);
            has_input_path = 1;
        }
    }

    /*
     * schema_dir, data_dir는 항상 필요합니다.
     */
    if (config->schema_dir[0] == '\0' || config->data_dir[0] == '\0') {
        return 0;
    }

    if (mode_count > 1) {
        return 0;
    }

    if (has_input_path && mode_count > 0) {
        return 0;
    }

    if (!has_input_path && mode_count == 0) {
        return 0;
    }

    return !has_input_path || config->input_path[0] != '\0';
}

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

static int is_exit_command(const char *line)
{
    return strcmp(line, ".exit") == 0 ||
           strcmp(line, "exit") == 0 ||
           strcmp(line, "quit") == 0;
}

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

int run_sql_file(const AppConfig *config, const char *path, ErrorInfo *error)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];

    if (!load_sql_file(path, sql_text, sizeof(sql_text), error)) {
        return 0;
    }

    return run_sql_text(config, sql_text, error);
}

int run_program(const AppConfig *config)
{
    ErrorInfo error;

    if (config->benchmark_mode) {
        if (!run_benchmark_mode(config, &error)) {
            print_error(&error);
            return 1;
        }

        return run_interactive_program(config);
    }

    if (config->interactive_mode) {
        return run_interactive_program(config);
    }

    if (!run_sql_file(config, config->input_path, &error)) {
        print_error(&error);
        return 1;
    }

    return 0;
}

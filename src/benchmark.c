#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "sqlproc.h"

#define BENCHMARK_PATH_LEN 512
#define BENCHMARK_INPUT_LEN 128

static void set_benchmark_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static double elapsed_ms(clock_t start_time, clock_t end_time)
{
    return ((double)(end_time - start_time) * 1000.0) / (double)CLOCKS_PER_SEC;
}

static void trim_line_end(char *line)
{
    size_t length;

    length = strlen(line);
    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[length - 1] = '\0';
        length -= 1;
    }
}

static int ensure_directory(const char *path)
{
    char buffer[BENCHMARK_PATH_LEN];
    size_t length;
    size_t i;

    if (path[0] == '\0') {
        return 0;
    }

    if (strlen(path) >= sizeof(buffer)) {
        return 0;
    }

    snprintf(buffer, sizeof(buffer), "%s", path);
    length = strlen(buffer);
    if (length == 0) {
        return 0;
    }

    for (i = 1; i < length; i++) {
        if (buffer[i] != '/') {
            continue;
        }

        buffer[i] = '\0';
        if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
            return 0;
        }
        buffer[i] = '/';
    }

    return mkdir(buffer, 0777) == 0 || errno == EEXIST;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file;

    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    if (fputs(text, file) == EOF) {
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

static int prompt_record_count(int *record_count, ErrorInfo *error)
{
    char line[BENCHMARK_INPUT_LEN];

    while (1) {
        char *end_ptr;
        long parsed_value;

        printf(">> 벤치마크를 위한 더미 데이터는 몇 개를 생성하시겠습니까? : ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            set_benchmark_error(error, "벤치마크 입력을 읽을 수 없습니다.");
            return 0;
        }

        trim_line_end(line);
        if (line[0] == '\0') {
            printf("양의 정수를 입력해 주세요.\n");
            continue;
        }

        errno = 0;
        parsed_value = strtol(line, &end_ptr, 10);
        if (errno == ERANGE || *end_ptr != '\0' ||
            parsed_value <= 0 || parsed_value > INT_MAX) {
            printf("양의 정수를 입력해 주세요.\n");
            continue;
        }

        *record_count = (int)parsed_value;
        return 1;
    }
}

static int prepare_benchmark_workspace(const AppConfig *config,
                                       char schema_dir[BENCHMARK_PATH_LEN],
                                       char data_dir[BENCHMARK_PATH_LEN],
                                       char pk_sql_path[BENCHMARK_PATH_LEN],
                                       char non_pk_sql_path[BENCHMARK_PATH_LEN],
                                       char csv_path[BENCHMARK_PATH_LEN],
                                       ErrorInfo *error)
{
    char benchmark_root[BENCHMARK_PATH_LEN];
    char sql_dir[BENCHMARK_PATH_LEN];

    snprintf(benchmark_root, sizeof(benchmark_root), "%s/benchmark", config->data_dir);
    snprintf(schema_dir, BENCHMARK_PATH_LEN, "%s/schemas", benchmark_root);
    snprintf(data_dir, BENCHMARK_PATH_LEN, "%s/data", benchmark_root);
    snprintf(sql_dir, sizeof(sql_dir), "%s/sql", benchmark_root);
    snprintf(pk_sql_path, BENCHMARK_PATH_LEN, "%s/pk_lookup.sql", sql_dir);
    snprintf(non_pk_sql_path, BENCHMARK_PATH_LEN, "%s/non_pk_lookup.sql", sql_dir);
    snprintf(csv_path, BENCHMARK_PATH_LEN, "%s/users.csv", data_dir);

    if (!ensure_directory(schema_dir) ||
        !ensure_directory(data_dir) ||
        !ensure_directory(sql_dir)) {
        set_benchmark_error(error, "벤치마크 작업 디렉터리를 만들 수 없습니다.");
        return 0;
    }

    return 1;
}

static int build_benchmark_files(const char *schema_dir,
                                 const char *csv_path,
                                 const char *pk_sql_path,
                                 const char *non_pk_sql_path,
                                 int record_count,
                                 int target_id,
                                 ErrorInfo *error)
{
    char schema_path[BENCHMARK_PATH_LEN];
    char pk_sql[SQLPROC_MAX_SQL_SIZE];
    char non_pk_sql[SQLPROC_MAX_SQL_SIZE];
    FILE *file;
    int i;

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        set_benchmark_error(error, "벤치마크 스키마 파일을 만들 수 없습니다.");
        return 0;
    }

    file = fopen(csv_path, "wb");
    if (file == NULL) {
        set_benchmark_error(error, "벤치마크 CSV 파일을 만들 수 없습니다.");
        return 0;
    }

    if (fputs("id,name,age\n", file) == EOF) {
        fclose(file);
        set_benchmark_error(error, "벤치마크 CSV 헤더를 쓸 수 없습니다.");
        return 0;
    }

    for (i = 1; i <= record_count; i++) {
        if (fprintf(file, "%d,user%d,%d\n", i, i, 20 + (i % 50)) < 0) {
            fclose(file);
            set_benchmark_error(error, "벤치마크 CSV 데이터를 쓸 수 없습니다.");
            return 0;
        }
    }

    fclose(file);

    snprintf(pk_sql, sizeof(pk_sql), "SELECT * FROM users WHERE id = %d;\n", target_id);
    snprintf(non_pk_sql,
             sizeof(non_pk_sql),
             "SELECT * FROM users WHERE name = 'user%d';\n",
             target_id);

    if (!write_text_file(pk_sql_path, pk_sql) ||
        !write_text_file(non_pk_sql_path, non_pk_sql)) {
        set_benchmark_error(error, "벤치마크 SQL 파일을 만들 수 없습니다.");
        return 0;
    }

    return 1;
}

static int run_sql_file_silently(const AppConfig *config,
                                 const char *path,
                                 double *elapsed,
                                 ErrorInfo *error)
{
    FILE *sink;
    int saved_stdout;
    int ok;
    clock_t start_time;
    clock_t end_time;

    sink = tmpfile();
    if (sink == NULL) {
        set_benchmark_error(error, "벤치마크 출력 캡처를 준비할 수 없습니다.");
        return 0;
    }

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        fclose(sink);
        set_benchmark_error(error, "벤치마크 출력 캡처를 준비할 수 없습니다.");
        return 0;
    }

    if (dup2(fileno(sink), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        fclose(sink);
        set_benchmark_error(error, "벤치마크 출력 캡처를 준비할 수 없습니다.");
        return 0;
    }

    start_time = clock();
    ok = run_sql_file(config, path, error);
    end_time = clock();

    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(sink);

    if (!ok) {
        return 0;
    }

    *elapsed = elapsed_ms(start_time, end_time);
    return 1;
}

static void print_benchmark_result_row(const char *label, double elapsed)
{
    printf("%-30s %10.3fms\n", label, elapsed);
}

/* 벤치마크는 sqlproc의 실제 파일 실행 경로를 그대로 사용해
 * cold / warm PK 조회와 non-PK 조회를 같은 조건에서 비교합니다.
 */
int run_benchmark_mode(const AppConfig *config, ErrorInfo *error)
{
    AppConfig benchmark_config;
    char schema_dir[BENCHMARK_PATH_LEN];
    char data_dir[BENCHMARK_PATH_LEN];
    char pk_sql_path[BENCHMARK_PATH_LEN];
    char non_pk_sql_path[BENCHMARK_PATH_LEN];
    char csv_path[BENCHMARK_PATH_LEN];
    double pk_cold_elapsed;
    double pk_warm_elapsed;
    double non_pk_elapsed;
    int record_count;
    int target_id;

    if (!prompt_record_count(&record_count, error)) {
        return 0;
    }

    if (!prepare_benchmark_workspace(config,
                                     schema_dir,
                                     data_dir,
                                     pk_sql_path,
                                     non_pk_sql_path,
                                     csv_path,
                                     error)) {
        return 0;
    }

    target_id = record_count * 9 / 10;
    if (target_id <= 0) {
        target_id = 1;
    }

    if (!build_benchmark_files(schema_dir,
                               csv_path,
                               pk_sql_path,
                               non_pk_sql_path,
                               record_count,
                               target_id,
                               error)) {
        return 0;
    }

    benchmark_config = *config;
    snprintf(benchmark_config.schema_dir, sizeof(benchmark_config.schema_dir), "%s", schema_dir);
    snprintf(benchmark_config.data_dir, sizeof(benchmark_config.data_dir), "%s", data_dir);
    benchmark_config.input_path[0] = '\0';
    benchmark_config.interactive_mode = 0;
    benchmark_config.benchmark_mode = 0;

    if (!run_sql_file_silently(&benchmark_config, pk_sql_path, &pk_cold_elapsed, error) ||
        !run_sql_file_silently(&benchmark_config, pk_sql_path, &pk_warm_elapsed, error) ||
        !run_sql_file_silently(&benchmark_config, non_pk_sql_path, &non_pk_elapsed, error)) {
        return 0;
    }

    printf("\n");
    printf("============= 벤치마크 결과 =============\n");
    print_benchmark_result_row("PK (id, cold)", pk_cold_elapsed);
    print_benchmark_result_row("PK (id, warm)", pk_warm_elapsed);
    print_benchmark_result_row("not PK (name)", non_pk_elapsed);
    printf("========================================\n\n");
    return 1;
}

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bptree.h"

#define DEFAULT_RECORD_COUNT 1000000
#define BENCH_LINE_LEN 128
#define BENCH_NAME_LEN 64

static double elapsed_ms(clock_t start, clock_t end)
{
    return ((double)(end - start) * 1000.0) / (double)CLOCKS_PER_SEC;
}

static int parse_record_count(int argc, char **argv, int *record_count)
{
    char *end_ptr;
    long parsed_value;

    *record_count = DEFAULT_RECORD_COUNT;
    if (argc == 1) {
        return 1;
    }

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: ./build/bench_index [record-count] [csv-path]\n");
        return 0;
    }

    errno = 0;
    parsed_value = strtol(argv[1], &end_ptr, 10);
    if (errno == ERANGE || *end_ptr != '\0' ||
        parsed_value <= 0 || parsed_value > INT_MAX) {
        fprintf(stderr, "record-count must be a positive int\n");
        return 0;
    }

    *record_count = (int)parsed_value;
    return 1;
}

static int write_records_and_build_index(const char *path,
                                         int record_count,
                                         BPlusTree *index,
                                         double *elapsed)
{
    FILE *file;
    clock_t start;
    clock_t end;
    int i;

    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "failed to create benchmark csv\n");
        return 0;
    }

    start = clock();
    if (fputs("id,name,age\n", file) == EOF) {
        fclose(file);
        return 0;
    }

    for (i = 1; i <= record_count; i++) {
        long offset;

        offset = ftell(file);
        if (offset < 0) {
            fclose(file);
            return 0;
        }

        if (fprintf(file, "%d,user%d,%d\n", i, i, 20 + (i % 50)) < 0) {
            fclose(file);
            return 0;
        }

        if (!bptree_insert(index, i, offset)) {
            fclose(file);
            return 0;
        }
    }

    end = clock();
    fclose(file);
    *elapsed = elapsed_ms(start, end);
    return 1;
}

static int scan_by_name(const char *path, const char *target_name, long *out_offset)
{
    FILE *file;
    char line[BENCH_LINE_LEN];

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 0;
    }

    while (1) {
        char name[BENCH_NAME_LEN];
        long offset;
        int id;
        int age;

        offset = ftell(file);
        if (offset < 0) {
            fclose(file);
            return 0;
        }

        if (fgets(line, sizeof(line), file) == NULL) {
            break;
        }

        if (sscanf(line, "%d,%63[^,],%d", &id, name, &age) != 3) {
            fclose(file);
            return 0;
        }

        if (strcmp(name, target_name) == 0) {
            *out_offset = offset;
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    BPlusTree *index;
    double insert_elapsed;
    double index_elapsed;
    double scan_elapsed;
    clock_t start;
    clock_t end;
    long indexed_offset;
    long scanned_offset;
    char target_name[BENCH_NAME_LEN];
    int record_count;
    int target_id;
    int found_index;
    int found_scan;

    if (!parse_record_count(argc, argv, &record_count)) {
        return 1;
    }

    path = argc >= 3 ? argv[2] : "bench-users.csv";
    target_id = record_count * 9 / 10;
    if (target_id <= 0) {
        target_id = record_count;
    }
    indexed_offset = -1;
    scanned_offset = -1;
    snprintf(target_name, sizeof(target_name), "user%d", target_id);

    index = bptree_create();
    if (index == NULL) {
        fprintf(stderr, "failed to create B+ Tree\n");
        return 1;
    }

    if (!write_records_and_build_index(path, record_count, index, &insert_elapsed)) {
        bptree_destroy(index);
        fprintf(stderr, "failed to build benchmark data\n");
        return 1;
    }

    start = clock();
    found_index = bptree_search(index, target_id, &indexed_offset);
    end = clock();
    index_elapsed = elapsed_ms(start, end);

    start = clock();
    found_scan = scan_by_name(path, target_name, &scanned_offset);
    end = clock();
    scan_elapsed = elapsed_ms(start, end);

    printf("records: %d\n", record_count);
    printf("csv path: %s\n", path);
    printf("target id: %d\n", target_id);
    printf("insert + index build elapsed: %.3f ms\n\n", insert_elapsed);

    printf("[INDEX] SELECT * FROM users WHERE id = %d\n", target_id);
    printf("found: %s, offset: %ld\n", found_index ? "yes" : "no", indexed_offset);
    printf("elapsed: %.6f ms\n\n", index_elapsed);

    printf("[SCAN] SELECT * FROM users WHERE name = '%s'\n", target_name);
    printf("found: %s, offset: %ld\n", found_scan ? "yes" : "no", scanned_offset);
    printf("elapsed: %.3f ms\n", scan_elapsed);

    bptree_destroy(index);
    return found_index && found_scan ? 0 : 1;
}

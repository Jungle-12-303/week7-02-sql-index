CC = cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Iinclude
TEST_CFLAGS = $(CFLAGS) -Wno-format-truncation
BUILD_DIR = build
SRC_DIR = src
TEST_DIR = tests
BENCH_DIR = benchmarks
HEADERS = $(wildcard include/*.h)
.DEFAULT_GOAL := all

# 앱 바이너리는 src 아래의 모든 .c 파일을 함께 빌드합니다.
APP_SRCS = $(wildcard $(SRC_DIR)/*.c)

# 테스트 바이너리는 main.c만 빼고 같은 코어 로직을 재사용합니다.
CORE_SRCS = $(filter-out $(SRC_DIR)/main.c, $(APP_SRCS))

APP_OBJS = $(APP_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CORE_OBJS = $(CORE_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TEST_OBJS = $(BUILD_DIR)/test_runner.o
BENCH_OBJS = $(BUILD_DIR)/bench_index.o

APP = $(BUILD_DIR)/sqlproc
TEST_APP = $(BUILD_DIR)/test_runner
BENCH_INDEX = $(BUILD_DIR)/bench_index
RECORDS ?= 1000000
DATA_PATH ?= demo-data/users.csv

.PHONY: all test bench seed-demo-data clean

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

all: $(APP)

$(APP): $(APP_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(APP_OBJS)

$(TEST_APP): $(CORE_OBJS) $(TEST_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(TEST_OBJS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_runner.o: $(TEST_DIR)/test_runner.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/bench_index.o: $(BENCH_DIR)/bench_index.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_APP)
	./$(TEST_APP)

$(BENCH_INDEX): $(BUILD_DIR)/bptree.o $(BENCH_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(BUILD_DIR)/bptree.o $(BENCH_OBJS)

bench: $(APP)
	./$(APP) --schema-dir ./examples/schemas --data-dir ./demo-data --benchmark

seed-demo-data: $(BENCH_INDEX)
	mkdir -p demo-data
	./$(BENCH_INDEX) $(RECORDS) $(DATA_PATH)

clean:
	rm -rf $(BUILD_DIR)

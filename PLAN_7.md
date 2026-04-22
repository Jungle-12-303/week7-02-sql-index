# PLAN_7.md

이 문서는 7주차 B+ Tree 인덱스 과제를 어떤 순서로 구현했는지와,
남은 개선 후보가 무엇인지 정리한 실행 기록입니다.
현재 프로젝트는 기존 SQL 처리기에 대화형 CLI, benchmark 모드, PK 자동 증가/중복 방지,
메모리 기반 B+ Tree 인덱스까지 들어간 상태입니다.

## 1. 현재 완료된 상태

- [x] Docker 기반 빌드 환경 확인
- [x] `make test` 통과
- [x] 대화형 CLI 모드 추가
  - 실행: `./build/sqlproc --schema-dir ./examples/schemas --data-dir ./demo-data --interactive`
  - 종료: `.exit`
- [x] `id:int` 컬럼을 PK로 인식
- [x] `INSERT` 시 `id` 생략 가능
  - 예: `INSERT INTO users (name, age) VALUES ('kim', 20);`
- [x] 자동 PK 증가
  - 기존 CSV의 `max(id)` 다음 값부터 발급
  - 실행 중에는 `next_id`를 메모리에 들고 증가
- [x] PK 중복 삽입 방지
  - 같은 `id`가 이미 B+ Tree에 있으면 `PK 값이 이미 존재합니다.` 에러
- [x] CSV 마지막 줄에 개행이 없을 때 append 전 자동 보정
- [x] B+ Tree 독립 모듈 구현
- [x] B+ Tree 단위 테스트 작성
- [x] INSERT 시 `id -> CSV row offset`을 B+ Tree에 등록
- [x] 프로그램 실행 중 테이블별 B+ Tree 인덱스 상태 관리
- [x] 테이블 최초 접근 시 CSV를 읽어 B+ Tree 재구성
- [x] `WHERE column = value`, `>`, `<`, `!=` 단일 조건 파싱
- [x] `WHERE id = ?`일 때 B+ Tree 인덱스 조회
- [x] `WHERE id > ?`, `WHERE id < ?`일 때 B+ Tree range scan
- [x] `WHERE name = 'kim'`처럼 PK가 아닌 조건은 CSV 선형 탐색
- [x] `[INDEX]`, `[INDEX-RANGE]`, `[SCAN]` 실행 로그 출력
- [x] 1,000,000개 이상 대량 데이터 benchmark 모드
- [x] README 발표용 정리

## 2. 아직 남은 개선 후보

- [ ] B+ Tree ORDER를 `4`에서 `32`, `64`, `128` 등으로 바꿔 성능 비교
- [ ] `BETWEEN` 기반 range scan 문법 추가
- [ ] CSV 대신 바이너리 파일 저장 방식 검토
- [ ] benchmark 측정값이 CPU time 기준임을 출력에 더 명확히 표시

## 3. 구현 순서 기록

### 1단계: 현재 상태 고정

목표는 지금까지 구현한 기능이 계속 통과하는지 확인하는 것입니다.

```bash
make
make test
```

확인할 것:

- 자동 PK가 동작하는지
- 중복 PK가 막히는지
- 대화형 CLI에서 `INSERT`, `SELECT *`가 되는지

시연용 데이터가 깨졌다면 먼저 초기화합니다.

```bash
rm -rf demo-data
mkdir demo-data
```

### 2단계: B+ Tree 독립 모듈 만들기

SQL 처리기와 연결하기 전에 B+ Tree만 따로 구현했습니다.

추가할 파일:

- `include/bptree.h`
- `src/bptree.c`

현재 API:

```c
typedef struct BPlusTree BPlusTree;
typedef int (*BptreeVisitFn)(int key, long offset, void *user_data);

BPlusTree *bptree_create(void);
void bptree_destroy(BPlusTree *tree);
int bptree_insert(BPlusTree *tree, int key, long offset);
int bptree_search(const BPlusTree *tree, int key, long *out_offset);
int bptree_visit_greater_than(const BPlusTree *tree,
                              int key,
                              BptreeVisitFn visit,
                              void *user_data);
int bptree_visit_less_than(const BPlusTree *tree,
                           int key,
                           BptreeVisitFn visit,
                           void *user_data);
```

정책:

- `key`: PK 값, 즉 `id`
- `value`: CSV row 시작 위치인 `offset`
- ORDER는 우선 `4`
- 중복 key insert는 실패 반환

### 3단계: B+ Tree 단위 테스트 작성

`tests/test_runner.c`에 B+ Tree 테스트를 추가했습니다.

필수 테스트:

- 1개 key 삽입 후 검색
- 여러 key 삽입 후 검색
- 정렬되지 않은 순서로 삽입
- 없는 key 검색 실패
- 중복 key 삽입 실패
- ORDER 4 기준 split 발생
- range scan을 위한 leaf 연결 유지
- 1,000개 정도 삽입 후 전부 검색

이 단계의 완료 기준:

```bash
make test
```

결과:

```text
All tests passed.
```

### 4단계: CSV append offset 확보

B+ Tree에는 row 전체가 아니라 CSV row 시작 위치만 저장합니다.

현재 구조:

```text
storage_append_row(...)
-> CSV에 row 한 줄 append
```

수정 방향:

```text
storage_append_row(...)
-> row 쓰기 직전 ftell(fp)로 offset 확보
-> CSV에 row append
-> out_offset으로 호출자에게 반환
```

권장 시그니처:

```c
int storage_append_row(const AppConfig *config,
                       const TableSchema *schema,
                       char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                       long *out_offset,
                       ErrorInfo *error);
```

주의:

- `ftell(fp)`은 `write_csv_row()` 호출 전에 해야 합니다.
- 파일 끝 개행 보정 후의 위치를 offset으로 써야 합니다.
- 기존 호출부와 테스트를 함께 수정해야 합니다.

### 5단계: 테이블별 인덱스 상태 만들기

현재 executor에는 PK 자동 증가용 상태가 있습니다.
여기에 B+ Tree 포인터를 함께 붙이거나 별도 상태 구조체를 만듭니다.

추천 구조:

```c
typedef struct {
    int in_use;
    char schema_dir[256];
    char data_dir[256];
    char table_name[SQLPROC_MAX_NAME_LEN];
    int next_id;
    BPlusTree *index;
    int index_built;
} TableRuntimeState;
```

이 상태는 실행 중에만 유지됩니다.

### 6단계: CSV에서 B+ Tree 재구성

메모리 기반 인덱스는 프로그램을 끄면 사라집니다.
따라서 테이블을 처음 사용할 때 CSV를 읽어 자동으로 다시 만들어야 합니다.

흐름:

```text
users.csv 열기
헤더 한 줄 읽기
while row 읽기:
    offset = row 시작 위치
    id = row의 PK 컬럼 값
    bptree_insert(index, id, offset)
    max_id 갱신
next_id = max_id + 1
```

구현 위치 후보:

- `executor.c`
  - 테이블 런타임 상태 관리
- `storage.c`
  - CSV row를 순회하며 `id`, `offset`을 넘겨주는 헬퍼

권장 함수:

```c
int storage_rebuild_pk_index(const AppConfig *config,
                             const TableSchema *schema,
                             BPlusTree *index,
                             int *max_id,
                             ErrorInfo *error);
```

### 7단계: INSERT 시 B+ Tree 등록

INSERT 최종 흐름은 아래처럼 바꿉니다.

```text
1. schema 로드
2. row_values 구성
3. PK 중복 검사
4. CSV append
5. append된 row offset 획득
6. B+ Tree에 id -> offset 삽입
```

주의:

- CSV 쓰기가 성공한 뒤 B+ Tree에 넣습니다.
- B+ Tree 삽입이 실패하면 에러를 반환합니다.
- 중복 검사는 이후 B+ Tree 기반으로 바꿀 수 있습니다.

### 8단계: WHERE 토큰/파서 추가

지원 범위는 단일 조건입니다.

지원:

```sql
SELECT * FROM users WHERE id = 1;
SELECT name, age FROM users WHERE id = 1;
SELECT * FROM users WHERE id > 1;
SELECT * FROM users WHERE id < 10;
SELECT * FROM users WHERE age != 20;
SELECT * FROM users WHERE name = 'kim';
```

지원하지 않음:

```sql
WHERE id BETWEEN 1 AND 10
WHERE name = 'kim' AND age = 20
```

수정 파일:

- `include/sqlproc.h`
  - `TOKEN_KEYWORD_WHERE`
  - `TOKEN_EQUAL`
  - `TOKEN_GREATER`
  - `TOKEN_LESS`
  - `TOKEN_BANG_EQUAL`
  - `WhereOperator`
  - `SelectStatement`에 where 필드 추가
- `src/tokenizer.c`
  - `where` 키워드
  - `=`, `>`, `<`, `!=` 토큰
- `src/parser.c`
  - `FROM table` 뒤에 선택적으로 `WHERE column op literal` 파싱

현재 구조:

```c
typedef struct {
    int has_where;
    char where_column[SQLPROC_MAX_NAME_LEN];
    SourceLocation where_column_location;
    WhereOperator where_operator;
    LiteralValue where_value;
} SelectStatement;
```

### 9단계: SELECT 실행 분기

SELECT 실행은 세 갈래로 나눕니다.

```text
WHERE 없음
-> 기존 storage_print_rows()

WHERE id = 숫자
-> [INDEX] 로그 출력
-> B+ Tree 검색
-> offset 획득
-> fseek으로 해당 row만 읽어 출력

WHERE id > 숫자 또는 WHERE id < 숫자
-> [INDEX-RANGE] 로그 출력
-> B+ Tree leaf range scan
-> offset 목록 획득
-> 해당 row만 읽어 출력

WHERE id가 아닌 컬럼
-> [SCAN] 로그 출력
-> CSV 전체 선형 탐색
-> 조건에 맞는 row 출력
```

추가할 스토리지 함수 후보:

```c
int storage_print_row_at_offset(const AppConfig *config,
                                const TableSchema *schema,
                                long offset,
                                const int selected_indices[SQLPROC_MAX_COLUMNS],
                                int selected_count,
                                ErrorInfo *error);

int storage_print_rows_where_equals(const AppConfig *config,
                                    const TableSchema *schema,
                                    const int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int selected_count,
                                    int where_column_index,
                                    WhereOperator where_operator,
                                    const LiteralValue *where_value,
                                    ErrorInfo *error);

int storage_print_rows_at_offsets(const AppConfig *config,
                                  const TableSchema *schema,
                                  const long offsets[],
                                  int offset_count,
                                  const int selected_indices[SQLPROC_MAX_COLUMNS],
                                  int selected_count,
                                  ErrorInfo *error);
```

### 10단계: 기능 테스트 추가

추가할 테스트:

- `SELECT * FROM users WHERE id = 1`
- `SELECT * FROM users WHERE id > 1`
- `SELECT * FROM users WHERE id < 10`
- `SELECT name, age FROM users WHERE id = 1`
- `SELECT * FROM users WHERE name = 'kim'`
- `SELECT * FROM users WHERE age != 20`
- 없는 id 조회
- 없는 name 조회
- `WHERE id = 'kim'` 타입 오류
- `WHERE unknown = 1` 컬럼 오류
- `[INDEX]` 로그 출력 확인
- `[SCAN]` 로그 출력 확인
- CSV 재구성 후 `WHERE id = ?` 조회 성공

### 11단계: 대량 데이터 벤치마크

100만 개 SQL 문장을 파일로 만드는 방식은 느리고 파일도 커집니다.
현재는 `sqlproc --benchmark`가 더미 CSV와 조회 SQL을 만들고,
같은 파일 실행 경로로 PK/non-PK 조회 시간을 잽니다.
`benchmarks/bench_index.c`는 보조 데이터 생성 도구로 남아 있습니다.

관련 파일:

- `src/benchmark.c`
- `benchmarks/bench_index.c`

측정 항목:

- `PK (id, cold)`: 첫 PK 조회와 인덱스 재구성 비용
- `PK (id, warm)`: 메모리에 재구성된 인덱스를 재사용한 조회
- `not PK (name)`: `name = 'user900000'` 선형 탐색 시간

출력 예:

```text
============= 벤치마크 결과 =============
PK (id, cold)                    309.519ms
PK (id, warm)                      0.060ms
not PK (name)                     91.877ms
========================================
```

### 12단계: README 발표용 정리

README에는 발표 때 바로 설명할 수 있는 내용만 넣습니다.

넣을 내용:

- 프로젝트 목표
- 기존 CSV 기반 SQL 처리기 구조
- 자동 PK 흐름
- B+ Tree key/value 구조
- `id -> CSV offset` 설명
- `WHERE id = ?` 인덱스 조회 흐름
- 다른 컬럼 조건의 선형 탐색 흐름
- 빌드/테스트/실행 방법
- 벤치마크 결과 표
- 한계와 개선 방향

## 4. 바로 다음 작업

핵심 구현은 완료되어 있으므로, 다음 작업은 품질과 발표 안정성 보강입니다.

1. `make test`와 `make bench`를 발표 환경에서 재실행
2. benchmark 수치가 CPU time 기준임을 발표 자료에 명확히 표시
3. B+ Tree ORDER 값을 키워 성능 차이를 비교할지 결정
4. 시간이 남으면 `BETWEEN` range scan 문법 추가 검토

즉, 다음 한 덩어리 목표는 이것입니다.

```text
이미 동작하는 B+ Tree 인덱스 흐름을 발표 환경에서 재현 가능하게 만든다.
```

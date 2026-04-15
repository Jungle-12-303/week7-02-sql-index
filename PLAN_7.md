# PLAN_7.md

이 문서는 7주차 B+ Tree 인덱스 과제를 앞으로 어떤 순서로 구현할지 정리한 실행 계획입니다.
현재 프로젝트는 기존 SQL 처리기에 대화형 CLI와 PK 자동 증가/중복 방지까지 들어간 상태입니다.

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
  - 같은 `id`가 이미 CSV에 있으면 `PK 값이 이미 존재합니다.` 에러
- [x] CSV 마지막 줄에 개행이 없을 때 append 전 자동 보정

## 2. 아직 남은 핵심 작업

- [ ] B+ Tree 독립 모듈 구현
- [ ] B+ Tree 단위 테스트 작성
- [ ] INSERT 시 `id -> CSV row offset`을 B+ Tree에 등록
- [ ] 프로그램 실행 중 테이블별 B+ Tree 인덱스 상태 관리
- [ ] 프로그램 시작 또는 테이블 최초 접근 시 CSV를 읽어 B+ Tree 재구성
- [ ] `WHERE column = value` 단일 조건 파싱
- [ ] `WHERE id = ?`일 때 B+ Tree 인덱스 조회
- [ ] `WHERE name = 'kim'`처럼 PK가 아닌 조건은 CSV 선형 탐색
- [ ] `[INDEX]`, `[SCAN]` 실행 로그 출력
- [ ] 1,000,000개 이상 대량 데이터 벤치마크
- [ ] README 발표용 정리

## 3. 구현 순서

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

SQL 처리기와 연결하기 전에 B+ Tree만 따로 구현합니다.

추가할 파일:

- `include/bptree.h`
- `src/bptree.c`

처음 API:

```c
typedef struct BPlusTree BPlusTree;

BPlusTree *bptree_create(void);
void bptree_destroy(BPlusTree *tree);
int bptree_insert(BPlusTree *tree, int key, long offset);
int bptree_search(BPlusTree *tree, int key, long *out_offset);
```

정책:

- `key`: PK 값, 즉 `id`
- `value`: CSV row 시작 위치인 `offset`
- ORDER는 우선 `4`
- 중복 key insert는 실패 반환

### 3단계: B+ Tree 단위 테스트 작성

`tests/test_runner.c`에 B+ Tree 테스트를 추가합니다.

필수 테스트:

- 1개 key 삽입 후 검색
- 여러 key 삽입 후 검색
- 정렬되지 않은 순서로 삽입
- 없는 key 검색 실패
- 중복 key 삽입 실패
- ORDER 4 기준 split 발생
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

지원 범위는 단일 조건만입니다.

지원:

```sql
SELECT * FROM users WHERE id = 1;
SELECT name, age FROM users WHERE id = 1;
SELECT * FROM users WHERE name = 'kim';
```

지원하지 않음:

```sql
WHERE id > 1
WHERE id BETWEEN 1 AND 10
WHERE name = 'kim' AND age = 20
```

수정 파일:

- `include/sqlproc.h`
  - `TOKEN_KEYWORD_WHERE`
  - `TOKEN_EQUAL`
  - `SelectStatement`에 where 필드 추가
- `src/tokenizer.c`
  - `where` 키워드
  - `=` 토큰
- `src/parser.c`
  - `FROM table` 뒤에 선택적으로 `WHERE column = literal` 파싱

추천 구조:

```c
typedef struct {
    int has_where;
    char where_column[SQLPROC_MAX_NAME_LEN];
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
                                    const LiteralValue *where_value,
                                    ErrorInfo *error);
```

### 10단계: 기능 테스트 추가

추가할 테스트:

- `SELECT * FROM users WHERE id = 1`
- `SELECT name, age FROM users WHERE id = 1`
- `SELECT * FROM users WHERE name = 'kim'`
- 없는 id 조회
- 없는 name 조회
- `WHERE id = 'kim'` 타입 오류
- `WHERE unknown = 1` 컬럼 오류
- `[INDEX]` 로그 출력 확인
- `[SCAN]` 로그 출력 확인
- CSV 재구성 후 `WHERE id = ?` 조회 성공

### 11단계: 대량 데이터 벤치마크

100만 개 SQL 문장을 파일로 만드는 방식은 느리고 파일도 커집니다.
벤치마크 전용 C 실행 파일을 두는 것이 좋습니다.

추천 파일:

- `benchmarks/bench_index.c`

측정 항목:

- 1,000,000건 INSERT 시간
- `WHERE id = 900000` 인덱스 조회 시간
- `WHERE name = 'user900000'` 선형 탐색 시간

출력 예:

```text
records: 1000000
insert elapsed: 12345.000 ms

[INDEX] SELECT * FROM users WHERE id = 900000
elapsed: 0.031 ms

[SCAN] SELECT * FROM users WHERE name = 'user900000'
elapsed: 487.210 ms
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

지금 바로 이어서 할 작업은 아래 순서가 좋습니다.

1. `include/bptree.h`, `src/bptree.c` 생성
2. B+ Tree `create/destroy/search/insert` 구현
3. B+ Tree 단위 테스트 추가
4. `make test` 통과
5. `storage_append_row`가 CSV offset을 반환하도록 수정
6. INSERT 성공 후 B+ Tree에 `id -> offset` 등록

즉, 다음 한 덩어리 목표는 이것입니다.

```text
SQL과 연결하기 전, B+ Tree 자체를 믿을 수 있게 만든다.
```

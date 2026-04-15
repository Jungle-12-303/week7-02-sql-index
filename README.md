# 7주차 B+ Tree 인덱스 발표 자료

## 목표

본 프로젝트는 기존 CSV 기반 SQL 처리기에 메모리 기반 B+ Tree 인덱스를 연동한 구현입니다.  
목표는 `WHERE id = ?`처럼 PK를 기준으로 한 조회에서 선형 탐색 대신 B+ Tree 인덱스를 사용하도록 만드는 것입니다.  
스키마에 `id:int` 컬럼이 있으면 이를 PK로 간주하고, B+ Tree에는 row 전체가 아니라 `id -> CSV row offset`만 저장합니다.

```text
key   = id
value = CSV 파일에서 해당 row가 시작되는 위치
```

즉 CSV는 실제 데이터를 보관하고, B+ Tree는 그 데이터로 빠르게 찾아가기 위한 주소록 역할을 합니다.

---

## 1. 요구사항 대응 요약과 B+ Tree 핵심

| 요구사항 | 구현 내용 |
| --- | --- |
| 자동 ID 부여 | `id`가 빠진 INSERT는 현재 최대 id 다음 값을 자동으로 채웁니다. |
| B+ Tree 인덱스 등록 | CSV 저장 후 `id -> row offset`을 B+ Tree에 등록합니다. |
| ID 기준 SELECT | `WHERE id = ?`는 `[INDEX]` 경로로 처리합니다. |
| 다른 필드 SELECT | `WHERE name`, `WHERE age`, `id != ?`는 `[SCAN]` 경로로 처리합니다. |
| 대용량 테스트 | `make seed-demo-data RECORDS=1000000`로 100만 건 CSV를 생성합니다. |

B+ Tree는 `bptree.c`에 독립 모듈로 구현했습니다.  
key는 `id`, value는 CSV row offset이며, 중복 key는 허용하지 않습니다.

<img width="5200" height="2732" alt="image" src="https://github.com/user-attachments/assets/6cfce597-0ea3-4630-8ac1-804aabbfa5eb" />

```text
bptree_insert()  : key를 정렬된 leaf node에 삽입
bptree_search()  : id로 offset 1개 검색
leaf split       : leaf가 가득 차면 오른쪽 leaf를 만들고 key를 나눔
internal split   : 부모 node도 가득 차면 split 후 promoted key를 위로 올림
leaf next        : id > ?, id < ? 범위 조회에서 순차 이동에 사용
```

이 구현 덕분에 `WHERE id = ?`는 tree 높이만큼만 이동해 offset을 찾고, `WHERE id > ?`, `WHERE id < ?`는 leaf 연결을 따라 필요한 offset들을 모을 수 있습니다.

---

## 2. 전체 구조

기존 SQL 처리기의 흐름은 유지하고, 실행 단계에서만 B+ Tree와 CSV 스토리지를 함께 사용합니다.  
즉 파서까지는 SQL을 구조체로 바꾸는 역할이고, `executor.c`에서 인덱스를 사용할지 선형 탐색을 할지 결정합니다.

```mermaid
flowchart LR
    A["SQL 입력"] --> B["tokenizer.c<br/>토큰 생성"]
    B --> C["parser.c<br/>Statement 생성"]
    C --> D["executor.c<br/>실행 경로 결정"]
    D --> E["storage.c<br/>CSV 읽기/쓰기"]
    D --> F["bptree.c<br/>id -> offset 검색"]
```

---

## 3. INSERT 실행 시 B+ Tree 적재 흐름

INSERT는 파서가 만든 `InsertStatement`를 `execute_insert()`가 받아 처리합니다.  
사용자가 컬럼 순서를 바꿔 입력해도 `build_insert_row_values()`에서 스키마 순서로 다시 맞추고, `id`가 빠져 있으면 자동 PK를 채웁니다.  
CSV에 쓰기 직전 `storage_append_row()`가 `ftell()`로 row offset을 구하고, 저장이 끝나면 `bptree_insert(id, offset)`으로 인덱스를 갱신합니다.

```mermaid
flowchart LR
    A["execute_program()<br/>SQL 문장을 순서대로 실행"] --> B["execute_insert()<br/>INSERT 실행 전체 제어"]
    B --> C["load_table_schema()<br/>컬럼 순서와 타입 확인"]
    C --> D["build_insert_row_values()<br/>입력 값을 스키마 순서로 재배치"]
    D --> E["validate_primary_key_unique()<br/>중복 id 여부 검사"]
    E --> F["storage_append_row()<br/>CSV 끝에 저장하고 ftell()로 offset 획득"]
    F --> G["bptree_insert(id, offset)<br/>id로 CSV 위치를 찾도록 인덱스 등록"]
```

```text
CSV 저장 순서 = INSERT 순서
B+ Tree 정렬 순서 = id 순서
```

---

## 4. SELECT 실행 시 Index / Scan 분기

SELECT는 `execute_select()`에서 먼저 스키마와 출력 컬럼을 확인합니다.  
이후 WHERE 조건이 PK인 `id`를 대상으로 하는지 보고 실행 방식이 갈라집니다.  
`WHERE id = ?`는 B+ Tree에서 offset 하나를 찾고, `WHERE id > ?`, `WHERE id < ?`는 leaf 연결을 따라 range scan을 합니다.  
반면 `name`, `age` 같은 일반 컬럼 조건이나 `id != ?`는 인덱스를 쓰지 않고 CSV를 처음부터 끝까지 읽습니다.

```mermaid
flowchart TD
    A["execute_select()<br/>SELECT 실행 방식 결정"] --> B["resolve_selected_columns()<br/>출력할 컬럼을 스키마 인덱스로 변환"]
    B --> C{"WHERE 있음?"}
    C -->|No| D["storage_print_rows()<br/>CSV 전체 row 출력"]
    C -->|Yes| E["resolve_where_column()<br/>WHERE 컬럼과 값 타입 검증"]
    E --> F{"PK id 조건?"}
    F -->|"id = ?"| G["execute_select_with_index()<br/>단일 PK 조회 경로"]
    G --> H["bptree_search()<br/>id로 row offset 1개 검색"]
    H --> I["storage_print_row_at_offset()<br/>fseek()로 해당 row만 읽기"]
    F -->|"id > ? / id < ?"| J["execute_select_with_index_range()<br/>PK 범위 조회 경로"]
    J --> K["bptree_visit_*()<br/>leaf 연결을 따라 offset 수집"]
    K --> L["storage_print_rows_at_offsets()<br/>수집한 row들만 읽기"]
    F -->|"그 외"| M["execute_select_with_scan()<br/>인덱스 없이 선형 탐색 선택"]
    M --> N["storage_print_rows_where_equals()<br/>CSV를 끝까지 읽으며 조건 비교"]
```

---

## 5. Full Scan과 Index 방식 차이

`id` 조건은 B+ Tree가 CSV 위치를 바로 알려주기 때문에 필요한 row만 `fseek()`로 읽습니다.  
반대로 `age`, `name` 조건은 인덱스가 없으므로 CSV 첫 row부터 마지막 row까지 파싱하고 비교합니다.

```text
[INDEX]       WHERE id = 900000
[INDEX-RANGE] WHERE id > 999990
[SCAN]        WHERE name = 'user900000'
elapsed: ... ms
```

결과가 적은 PK 조회에서는 인덱스 효과가 크고, 결과가 거의 전체 row인 조건은 출력 비용이 커서 차이가 줄어듭니다. 이 차이를 선택도(selectivity)라고 설명할 수 있습니다.

---

## 6. 왜 B+ Tree를 쓰는가

단순히 CSV를 처음부터 끝까지 읽으면 원하는 `id` 하나를 찾기 위해 모든 row를 확인해야 합니다.  
반대로 B+ Tree는 key가 정렬된 트리 구조라서 root에서 leaf까지 필요한 경로만 따라가면 됩니다.  
데이터가 많아질수록 전체 row 수를 모두 보는 방식과, tree 높이만큼만 이동하는 방식의 차이가 커집니다.

```text
CSV 선형 탐색 : row 1 -> row 2 -> row 3 -> ... -> target
B+ Tree 탐색 : root -> internal node -> leaf -> target offset
```

또 하나의 장점은 메모리 사용 방식입니다.  
CSV 전체를 메모리에 struct 배열로 올리면 검색은 파일 I/O보다 빨라질 수 있지만, 모든 컬럼 값을 메모리에 복사해야 합니다.

예를 들어 현재 `users` row를 C 구조체로 올린다고 생각하면 대략 아래와 같습니다.

```c
typedef struct {
    int id;          /* 4 bytes */
    char name[64];   /* 64 bytes */
    int age;         /* 4 bytes */
} UserRow;
```

이 방식은 row 하나당 최소 약 72 bytes가 필요하고, 1,000,000건이면 약 72MB 이상을 사용합니다.  
반면 현재 B+ Tree 인덱스는 row 전체를 저장하지 않고 `id`와 `offset`만 저장합니다.

```text
id      : int  4 bytes
offset  : long 8 bytes
payload : 약 12 bytes / row
```

실제 B+ Tree에는 node 포인터와 split 관리를 위한 추가 메모리가 붙습니다.  
그래도 핵심 차이는 명확합니다. struct 배열 방식은 `name`, `age` 같은 실제 데이터를 모두 복사하지만, B+ Tree 방식은 “어디로 가면 되는지”만 기억합니다.  
즉 데이터 본문은 CSV에 두고, 메모리에는 검색에 필요한 최소 정보만 올리는 구조입니다.

| 방식 | 메모리에 올리는 것 | 장점 | 단점 |
| --- | --- | --- | --- |
| CSV 선형 탐색 | 거의 없음 | 구현이 단순함 | 매번 전체 row를 읽음 |
| CSV 전체 struct 적재 | 모든 row의 모든 컬럼 | 파일 I/O 없이 탐색 가능 | 메모리 사용량이 큼, 조건에 따라 여전히 선형 탐색 |
| B+ Tree 인덱스 | `id -> offset` | PK 조회가 빠르고 메모리 사용이 작음 | 인덱스 생성/유지 비용이 있음 |

---

## 7. 메모리 인덱스 재구성

이번 인덱스는 디스크에 저장하지 않는 메모리 기반 구조입니다.  
따라서 프로그램을 새로 실행하면 B+ Tree는 비어 있고, 기존 CSV 데이터와 다시 연결하는 과정이 필요합니다.

테이블을 처음 접근할 때 `get_table_state_index()`가 런타임 상태를 확인합니다.  
상태가 없으면 `create_table_state()`가 B+ Tree를 만들고, `storage_rebuild_pk_index()`가 CSV를 한 번 읽어 기존 row의 `id -> offset`을 다시 등록합니다.

```text
테이블 처음 접근
-> get_table_state_index()
-> create_table_state()
-> storage_rebuild_pk_index()
-> CSV의 기존 id와 offset을 B+ Tree에 재등록
```

이후 같은 실행 안에서는 이미 만들어진 B+ Tree를 재사용하므로, 두 번째 PK 조회부터는 재구성 비용 없이 바로 검색합니다.

---

## 검증과 마무리

- B+ Tree 삽입 / 검색 / split 테스트
- 자동 PK 증가와 중복 PK 방지 테스트
- `WHERE id` 인덱스 조회, `WHERE name`, `WHERE age` full scan 테스트
- 1,000,000건 CSV 생성 후 성능 비교

```bash
make test
make seed-demo-data RECORDS=1000000
./build/sqlproc --schema-dir ./examples/schemas --data-dir ./demo-data ./examples/perf_compare.sql
```

정리하면, 이번 구현은 기존 SQL 처리기 구조를 유지하면서 `executor.c`에서 조회 방식을 분기하도록 연결한 것이 핵심입니다.  
PK 조회는 B+ Tree로 빠르게 offset을 찾고, 그 외 조건은 CSV 선형 탐색으로 처리해 두 방식의 차이를 로그와 실행 시간으로 확인할 수 있습니다.




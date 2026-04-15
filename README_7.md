# 7주차 B+ Tree 인덱스 발표 자료

## 목표

기존 CSV 기반 SQL 처리기에 메모리 기반 B+ Tree 인덱스를 붙였습니다.  
핵심은 `id`를 PK로 보고, `id -> CSV row offset`을 B+ Tree에 저장하는 것입니다.

즉 B+ Tree에는 row 전체가 아니라 CSV에서 해당 row가 시작되는 위치만 저장합니다.

```text
key   = id
value = CSV row offset
```

---

## 1. INSERT 이후 B+ Tree 적재 흐름



INSERT는 CSV 파일 끝에 row를 append합니다.  
CSV 중간에 끼워 넣지 않고, append 직전 `ftell()`로 얻은 offset을 B+ Tree에 등록합니다.

```text
CSV 저장 순서 = INSERT 순서
B+ Tree 정렬 순서 = id 순서
```

---

## 2. SELECT 이후 B+ Tree 사용 흐름


메모리 인덱스라서 프로그램을 새로 켜면 B+ Tree는 비어 있습니다.  
처음 `WHERE id`를 쓰거나 INSERT에서 PK 상태가 필요할 때 `storage_rebuild_pk_index()`가 CSV를 한 번 읽어 `id -> offset`을 재구성합니다.

---

## 3. Full Scan과 Index 방식 차이

```mermaid
flowchart LR
    Q1["WHERE id = 900000"] --> I1["B+ Tree 탐색"]
    I1 --> I2["offset 1개 획득"]
    I2 --> I3["fseek()로 해당 row만 읽기"]
    I3 --> I4["[INDEX]"]

    Q2["WHERE age > 100"] --> S1["CSV 첫 row부터 읽기"]
    S1 --> S2["각 row 파싱"]
    S2 --> S3["조건 비교"]
    S3 --> S4["끝까지 반복"]
    S4 --> S5["[SCAN]"]
```

`id` 조건은 B+ Tree가 CSV 위치를 바로 알려줍니다.  
반면 `age`, `name` 같은 컬럼은 인덱스가 없어서 CSV 전체를 읽고 비교합니다.

---

## 데모 포인트

```sql
SELECT * FROM users WHERE id = 900000;
SELECT * FROM users WHERE name = 'user900000';
SELECT * FROM users WHERE id > 999990;
SELECT * FROM users WHERE age > 100;
```

출력 로그로 조회 방식을 바로 볼 수 있습니다.

```text
[INDEX]
[INDEX-RANGE]
[SCAN]
elapsed: ... ms
```

결과가 적은 `id = ?`, `id > ?` 조건에서는 인덱스 효과가 큽니다.  
반대로 결과가 거의 전체 row인 조건은 출력 비용이 커서 인덱스 효과가 줄어듭니다.  
이 차이를 선택도(selectivity)라고 설명할 수 있습니다.

---

## 검증

- B+ Tree 삽입 / 검색 / split 테스트
- 중복 PK 방지 테스트
- 자동 PK 증가 테스트
- `WHERE id` 인덱스 조회 테스트
- `WHERE age`, `WHERE name` full scan 테스트
- 1,000,000건 CSV 생성 후 성능 비교

```bash
make test
make seed-demo-data RECORDS=1000000
./build/sqlproc --schema-dir ./examples/schemas --data-dir ./demo-data ./examples/perf_compare.sql
```

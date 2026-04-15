# AGENTS_7.md

이 문서는 7주차 수요 코딩회 과제인 B+ Tree 인덱스 구현을 위한 작업 브리프입니다.
기존 C99 기반 SQL 처리기를 그대로 활용하면서, PK 기반 조회에 메모리 기반 B+ Tree 인덱스를 연결하는 것을 목표로 합니다.

## 1. 과제 목표

- 기존 SQL 처리기에 B+ Tree 인덱스를 추가합니다.
- `INSERT` 시 PK 값을 자동으로 부여합니다.
- `WHERE id = ?` 조건 조회에서는 B+ Tree 인덱스를 사용합니다.
- 그 외 컬럼 조건 조회에서는 CSV 전체 선형 탐색을 사용합니다.
- 1,000,000개 이상 레코드를 삽입하고 인덱스 조회와 선형 탐색 성능을 비교합니다.

## 2. 핵심 실행 방식

```text
INSERT
-> 자동 PK 부여
-> CSV에 레코드 저장
-> id와 CSV row offset을 B+ Tree에 등록

SELECT ... WHERE id = ?
-> B+ Tree에서 id 검색
-> CSV row offset 획득
-> fseek으로 해당 레코드 읽기

SELECT ... WHERE 다른 컬럼 = ?
-> CSV 파일 전체 선형 탐색
-> 조건에 맞는 레코드 출력

SELECT * FROM table
-> 전체 조회이므로 CSV 파일 전체 선형 탐색
```

## 3. 먼저 확정할 구현 정책

구현 중 해석이 갈리지 않도록 아래 정책을 우선 적용합니다.

- B+ Tree ORDER는 우선 `4`로 시작합니다.
  - 이유: split과 leaf 연결 구조를 테스트하기 쉽고, 초심자가 디버깅하기 좋습니다.
  - 성능 벤치마크 이후 `32`, `64`, `128` 등으로 조정할 수 있습니다.
- PK 중복 삽입은 에러로 처리합니다.
  - 같은 `id`가 이미 B+ Tree에 있으면 CSV에 쓰지 않고 실패를 반환합니다.
  - 자동 PK 모드에서는 기존 CSV를 읽어 가장 큰 `id` 이후 값부터 발급합니다.
- 메모리 기반 인덱스는 프로그램 시작 시 CSV를 전체 스캔해서 재구성합니다.
  - 인덱스는 디스크에 저장하지 않습니다.
  - `data/<table>.csv`를 한 줄씩 읽으며 `id`와 해당 줄의 file offset을 B+ Tree에 다시 등록합니다.
- `WHERE` 파싱은 단일 조건만 지원합니다.
  - 지원: `WHERE id = 123`
  - 지원: `WHERE name = 'kim'`
  - 지원하지 않음: `AND`, `OR`, `BETWEEN`, `>`, `<`, `!=`
- `WHERE id = 숫자`는 B+ Tree 인덱스를 사용합니다.
- `WHERE id`가 아닌 조건은 문자열 값 포함 여부와 관계없이 CSV 선형 탐색을 사용합니다.
- 기존 `REVIEW.md`에 P1 버그가 기록되어 있다면 B+ Tree 구현 전에 먼저 확인하고 수정합니다.
  - 특히 `executor.c`에서 `fgets` 버퍼 잘림으로 긴 CSV row를 잘못 읽는 문제가 있으면 1,000,000건 테스트 전에 해결해야 합니다.

## 4. CSV와 B+ Tree 연결 방식

CSV는 파일 포인터와 row offset을 기준으로 B+ Tree와 연결합니다.

```text
CSV 파일
offset 0    : id,name,age
offset 12   : 1,kim,20
offset 21   : 2,lee,30
offset 30   : 3,park,25

B+ Tree
1 -> 12
2 -> 21
3 -> 30
```

### INSERT 시 처리

1. CSV 파일을 append 모드로 연다.
2. 레코드를 쓰기 직전에 `ftell(fp)`로 현재 file offset을 얻는다.
3. CSV에 한 줄을 쓴다.
4. B+ Tree에 `id -> offset`을 삽입한다.

즉, B+ Tree에는 레코드 전체를 저장하지 않고 레코드 위치만 저장합니다.

### SELECT WHERE id 시 처리

1. B+ Tree에서 `id`를 검색한다.
2. 검색 결과로 CSV file offset을 얻는다.
3. `fseek(fp, offset, SEEK_SET)`으로 해당 위치로 이동한다.
4. CSV 한 줄을 읽어 SELECT 결과로 출력한다.

### SELECT WHERE 다른 컬럼 시 처리

1. CSV 파일을 처음부터 읽는다.
2. 헤더를 기준으로 조건 컬럼의 위치를 찾는다.
3. 각 row를 한 줄씩 읽어 조건 값과 비교한다.
4. 일치하는 row를 출력한다.

따라서 CSV 데이터를 함수 간에 전달할 때는 “전체 파일 포인터”를 넘기고, 인덱스 검색 결과로는 “한 줄의 시작 offset”만 넘기는 방식을 기본으로 합니다.
실제 row 데이터는 필요한 시점에 `fseek` 후 한 줄씩 읽습니다.

## 5. 구현 필요한 기능

- [ ] `INSERT`하는 경우 PK 값 자동 추가
- [ ] PK에 대해 `SELECT`하는 경우 인덱스 기반 조회
- [ ] 그 외 컬럼에 대해 `SELECT`하는 경우 선형 탐색
- [ ] 인덱스는 B+ Tree 알고리즘으로 구현
- [ ] 인덱스는 디스크 기반이 아닌 메모리 기반 방식으로 구현
- [ ] 대량 데이터 1,000,000개 이상 레코드를 쉽게 `INSERT`할 수 있는 방법 제공

## 6. 중점 포인트

- [ ] `WHERE id = ?` 조건, 즉 단일 PK 조건일 때 B+ Tree를 어떻게 사용할지 명확히 구현
- [ ] 대용량 레코드를 쉽게 생성하고 테스트할 수 있는 벤치마크 또는 데이터 생성 도구 마련
- [ ] 이전 차수에서 만든 SQL 처리기와 자연스럽게 연결

## 7. Codex에게 우선 시킬 일

1. 현재 SQL 처리기의 `INSERT`, `SELECT`, CSV 저장 흐름을 먼저 파악한다.
2. `REVIEW.md`의 P1 버그와 `executor.c`의 CSV row 읽기 안정성을 먼저 확인한다.
3. B+ Tree를 독립 모듈로 구현한다.
   - 추천 파일: `include/bptree.h`, `src/bptree.c`
   - 기본 API: 생성, 삽입, 검색, 해제
4. B+ Tree 단위 테스트를 먼저 작성한다.
5. `INSERT` 시 자동 PK를 부여하고, `id -> CSV row offset`을 B+ Tree에 등록한다.
6. 프로그램 시작 시 CSV를 한 줄씩 읽어 B+ Tree 인덱스를 재구성한다.
7. `SELECT ... WHERE id = ?`와 `SELECT ... WHERE name = 'kim'` 문법을 파서에 추가한다.
8. 실행부에서 `WHERE id = ?`는 B+ Tree 검색을 사용하고, 그 외 조건은 CSV 선형 탐색을 사용한다.
9. 1,000,000개 이상 레코드 삽입과 검색 성능 비교를 수행한다.
10. README에 실행 방법, 테스트 방법, 성능 비교 결과를 발표용으로 정리한다.

## 8. 추천 구현 순서

1. 기존 프로젝트 빌드와 테스트를 확인한다.
2. `REVIEW.md`의 P1 버그를 확인하고 필요한 경우 먼저 수정한다.
3. `INSERT`, `SELECT`, CSV 저장 흐름을 읽는다.
4. `include/bptree.h`, `src/bptree.c`를 만든다.
5. `ORDER=4`로 B+ Tree 단위 테스트를 작성한다.
6. B+ Tree 삽입, 검색, split을 구현한다.
7. CSV에 레코드를 쓰기 전 `ftell`로 row offset을 얻는다.
8. `INSERT` 시 자동 PK를 부여한다.
9. `id -> row offset` 값을 B+ Tree에 등록한다.
10. 프로그램 시작 또는 테이블 로딩 시 CSV를 읽어 B+ Tree를 재구성한다.
11. 파서에 `WHERE column = value` 단일 조건을 추가한다.
12. 문자열 조건 값, 예를 들어 `WHERE name = 'kim'`을 처리한다.
13. 실행부에서 `WHERE id = ?`와 그 외 조건을 분기한다.
14. 1,000,000개 이상 데이터를 생성하는 벤치마크를 만든다.
15. 인덱스 조회와 선형 탐색 성능을 비교한다.
16. README에 데모 방법과 성능 결과를 정리한다.

## 9. 성능 측정 기준

성능 측정은 발표에서 재현 가능한 방식으로 단순하게 유지합니다.

- 측정 단위는 기본적으로 `ms`를 사용합니다.
  - 매우 짧은 검색은 `us` 단위도 함께 출력할 수 있습니다.
- 측정 함수는 C99 호환성을 우선해 `clock()`을 기본으로 사용합니다.
  - POSIX 환경만 대상으로 확정되면 `clock_gettime()`을 검토합니다.
- 비교는 같은 목표 레코드를 대상으로 수행합니다.
  - 예: `id = 900000` 인덱스 조회
  - 예: `name = 'user900000'` 선형 탐색
- 출력에는 조회 방식이 반드시 드러나야 합니다.

```text
records: 1000000
target id: 900000

[INDEX] SELECT * FROM users WHERE id = 900000
elapsed: 0.031 ms

[SCAN] SELECT * FROM users WHERE name = 'user900000'
elapsed: 487.210 ms
```

## 10. 테스트 대상

- [ ] B+ Tree 단일 key 삽입 및 검색
- [ ] B+ Tree 다중 key 삽입 및 검색
- [ ] B+ Tree ORDER 4 기준 split 동작
- [ ] B+ Tree 노드 split 발생 케이스
- [ ] 존재하지 않는 key 검색
- [ ] PK 중복 삽입 시 에러 반환
- [ ] `INSERT` 시 PK 자동 증가 확인
- [ ] CSV 재로딩 후 B+ Tree 인덱스 재구성 확인
- [ ] `SELECT * FROM table WHERE id = ?`가 인덱스를 사용하는지 확인
- [ ] `SELECT * FROM table WHERE name = ?`처럼 PK가 아닌 조건은 선형 탐색하는지 확인
- [ ] `SELECT * FROM table WHERE name = 'kim'`처럼 문자열 조건이 동작하는지 확인
- [ ] 기존 `SELECT * FROM table` 전체 조회가 깨지지 않는지 확인
- [ ] 1,000,000개 이상 레코드 삽입 후 성능 측정

## 11. 수정 및 개선 후보

- [ ] 실행 로그에 `[INDEX]` 또는 `[SCAN]`을 출력해 어떤 조회 방식이 사용됐는지 확인 가능하게 만들기
- [ ] 성능 테스트 결과를 README 표로 정리하기
- [ ] B+ Tree ORDER를 4에서 더 큰 값으로 바꿔 성능 차이를 비교하기

## 12. 고민해 볼 추가 기능 우선순위

시간 대비 발표 효과를 기준으로 아래 순서를 우선합니다.

1. `[INDEX]` / `[SCAN]` 로그 출력
   - 발표 데모에서 인덱스 사용 여부를 바로 보여줄 수 있어 효과가 큽니다.
2. `BETWEEN` 기반 range scan
   - B+ Tree leaf node 연결리스트를 왜 두는지 설명하기 좋습니다.
3. CSV 대신 바이너리 파일 저장
   - 구현량이 커지므로 핵심 기능 완료 후 검토합니다.
4. malloc lab 결과물 적용
   - 차별점은 있지만 리스크가 크므로 마지막에 검토합니다.

## 13. 고민해 볼 추가 기능 상세

- [ ] CSV 파일 대신 바이너리 파일로 테이블 데이터 읽고 쓰기
- [ ] `WHERE`에 `AND`, `OR`, `BETWEEN` 연산자 추가
- [ ] 메모리 방식에 팀에서 만든 malloc lab 결과물을 적용할지 검토
- [ ] PK에 대해 `WHERE`를 사용하는 경우 인덱스 기반 조회
- [ ] 그 외 컬럼에 대해 `WHERE`를 사용하는 경우 선형 탐색
- [ ] B+ Tree leaf node 연결을 활용한 range scan 구현

## 14. 발표에서 강조할 내용

- `SELECT * FROM table`은 전체 조회이므로 선형 탐색이 자연스럽다.
- `SELECT * FROM table WHERE id = ?`는 특정 PK 하나를 찾는 조회이므로 B+ Tree 인덱스를 사용한다.
- 메모리 기반 인덱스이므로 프로그램 재실행 시 CSV를 읽어 인덱스를 재구성하는 방식이 필요하다.
- CSV row 자체를 B+ Tree에 저장하지 않고, `id -> file offset`만 저장해 파일 포인터로 해당 줄을 찾아간다.
- 성능 비교는 PK 기반 인덱스 조회와 다른 컬럼 기반 선형 탐색을 나란히 보여준다.

## 15. 발표 데모 추천 흐름

```bash
make
make test
./build/sqlproc --schema-dir ./examples/schemas --data-dir ./demo-data ./examples/index_demo.sql
./build/bench_index
```

발표 설명 순서는 아래처럼 짧게 잡습니다.

1. 기존 SQL 처리기는 CSV 전체 탐색 기반이었다.
2. 이번 구현에서는 `id`에 대해 B+ Tree 인덱스를 추가했다.
3. `INSERT` 시 자동 PK를 만들고, CSV offset을 B+ Tree에 저장한다.
4. `WHERE id = ?` 조회는 B+ Tree로 offset을 찾고 `fseek`으로 바로 읽는다.
5. 다른 컬럼 조건은 인덱스가 없으므로 선형 탐색한다.
6. 1,000,000개 기준 성능 비교 결과를 보여준다.

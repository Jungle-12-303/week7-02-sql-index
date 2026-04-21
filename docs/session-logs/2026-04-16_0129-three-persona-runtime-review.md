# 요청 요약

- 현재 브랜치 `feat/benchmark-mode`의 코드를 직접 실행한다.
- 기본 빌드, 자동 테스트, interactive 포함 수동 엣지 케이스, benchmark를 실제로 돌린다.
- `three-persona-multi-agent-review` 규칙에 맞춰
  - 시니어 코치
  - 주니어 서포터
  - 초심자 C 학생
  세 관점의 리뷰를 병렬로 수집하고, 합의점과 차이를 정리한다.

# 결정 사항

- 코드 변경 없이, 이번 검증과 리뷰 결과는 세션 로그로 남긴다.
- 실행 평가는 실제 CLI 경로를 기준으로 한다.
  - `make`
  - `make test`
  - `./build/sqlproc ... ./examples/demo.sql`
  - `./build/sqlproc ... ./examples/index_demo.sql`
  - `./build/sqlproc ... --interactive`
  - `./build/sqlproc ... --benchmark`
- 멀티 페르소나 리뷰는 같은 아티팩트를 세 에이전트에게 전달하고,
  최종 요약도 한국어로 정리한다.

# 현재 브랜치 상태

- 브랜치: `feat/benchmark-mode`
- 시작 시 작업 트리 상태: clean
- 검증 시작 기준 HEAD: `703665d` `docs: sqlproc 벤치마크 사용 흐름을 정리`

# 완료한 작업

- 기본 빌드 확인
  - `make`
  - 결과: 성공
- 자동 테스트 확인
  - `make test`
  - 결과: `All tests passed.`
- 배치 실행 확인
  - `./build/sqlproc --schema-dir ./examples/schemas --data-dir /tmp/sqlproc-demo-MtranK ./examples/demo.sql`
  - 결과: `INSERT`, 전체 `SELECT`, 컬럼 순서 재배치 출력이 정상 동작
- 인덱스 분기 확인
  - `./build/sqlproc --schema-dir ./examples/schemas --data-dir /tmp/sqlproc-edge-tvAyPe ./examples/index_demo.sql`
  - 결과:
    - `WHERE id = 2` -> `[INDEX]`
    - `WHERE id > 2`, `WHERE id < 3` -> `[INDEX-RANGE]`
    - `WHERE name = 'kim'`, `WHERE age != 20` -> `[SCAN]`
- interactive 수동 엣지 케이스 확인
  - `SELECT * FROM users WHERE id = 999;`
    - `[INDEX]` 로그 후 헤더만 출력
  - `SELECT * FROM users WHERE name = 'nobody';`
    - `[SCAN]` 로그 후 헤더만 출력
  - `SELECT * FROM users WHERE name > 'kim';`
    - `문자열 컬럼은 >, < 조건을 사용할 수 없습니다.`
  - `SELECT * FROM users WHERE id = 'kim';`
    - `WHERE 값 타입이 스키마와 맞지 않습니다.`
- 추가 수동 오류 케이스 확인
  - `INSERT INTO users VALUES (1, '=2+3', 20);`
    - `CSV에서 수식으로 해석될 수 없습니다.`
  - `INSERT INTO users VALUES (2147483648, 'kim', 20);`
    - `정수 값이 int 범위를 벗어났습니다.`
  - duplicate PK
    - 상위 `data_dir`가 없으면 `데이터 파일을 만들 수 없습니다.`
    - 디렉터리를 만든 뒤에는 `PK 값이 이미 존재합니다.`
- benchmark 실제 실행 확인
  - `./build/sqlproc --schema-dir ./examples/schemas --data-dir /tmp/sqlproc-bench-million-G5CEf2 --benchmark`
  - 입력: `1000000`
  - 생성 산출물:
    - `/tmp/sqlproc-bench-million-G5CEf2/benchmark/data/users.csv`
    - `/tmp/sqlproc-bench-million-G5CEf2/benchmark/sql/pk_lookup.sql`
    - `/tmp/sqlproc-bench-million-G5CEf2/benchmark/sql/non_pk_lookup.sql`
  - 생성 데이터 규모:
    - `1,000,001` lines
    - 약 `20M`
  - benchmark 출력:
    - `PK (id, cold) 307.824ms`
    - `PK (id, warm) 0.104ms`
    - `not PK (name) 90.266ms`
  - 전체 프로세스 wall time 관찰:
    - `real 7.42`
- benchmark SQL 단독 실행 재확인
  - `pk_lookup.sql`
    - `[INDEX] WHERE id = 900000`
    - `elapsed: 312.523 ms`
  - `non_pk_lookup.sql`
    - `[SCAN] WHERE name = user900000`
    - `elapsed: 94.456 ms`
- 손상 CSV 정책 추가 확인
  - zero-byte `users.csv`를 만든 뒤
    - `SELECT * FROM users;`는 성공처럼 종료되고 `elapsed`만 출력
    - `INSERT`는 `기존 데이터 파일 헤더를 읽을 수 없습니다.`로 실패

# 리뷰 결과

## Senior Coach

- 핵심 기능과 실제 실행 결과는 잘 맞는다.
- `INSERT`의 타입 검사, int 범위 검사, CSV 수식 주입 방지, PK 중복 방지는 correctness 관점에서 강하다.
- 다만 장기 리스크로 아래를 지적했다.
  - benchmark 수치가 `clock()` 기반 CPU time이라 wall-clock처럼 읽히면 오해가 생긴다.
  - 손상 CSV 처리 정책이 경로마다 다르다.
  - `static` 전역 `table_states`는 단발 CLI에는 괜찮지만 장기적으로 stale state와 해제 부재 리스크가 있다.

## Junior Supporter

- 발표용 데모와 팀 협업 기준에서 현재 브랜치는 안정적이라고 봤다.
- `app.c`의 모드 파싱과 `executor.c`의 `[INDEX]` / `[INDEX-RANGE]` / `[SCAN]` 분기가 특히 읽기 쉽다고 평가했다.
- 테스트가 실전형이라 회귀 방지에 도움이 된다고 봤다.
- 다만 아래는 설명을 더 보강하면 좋다고 봤다.
  - `data_dir/benchmark/...` 전제
  - `cold`가 인덱스 재구성 비용을 포함한다는 점
  - benchmark 숫자가 엄밀한 end-to-end wall time은 아니라는 점

## Beginner C Student

- 결과 로그 자체는 매우 교육적이라고 봤다.
  - 어떤 쿼리가 왜 `[INDEX]`인지, 왜 `[SCAN]`인지 눈에 들어온다.
- 초심자 입장에서는 아래가 특히 헷갈릴 수 있다고 봤다.
  - `row offset`이 “행 번호”가 아니라 파일 바이트 위치라는 점
  - `ftell()` / `fseek()`가 인덱스 조회와 어떻게 연결되는지
  - `cold`와 `warm`의 정확한 차이
  - 결과가 없을 때 헤더만 출력되는 정책
- README에 작은 예시 그림과 짧은 설명이 더 있으면 이해가 빨라질 것이라고 봤다.

## Combined Takeaway

- 세 관점이 모두 동의한 점
  - 현재 브랜치는 핵심 요구사항을 실제 실행에서 충실히 만족한다.
  - `[INDEX]`, `[INDEX-RANGE]`, `[SCAN]` 로그는 교육용 시연에 매우 유효하다.
  - warm PK 조회와 non-PK scan의 성능 차이는 발표 포인트로 충분히 강하다.
- 관점이 갈린 점
  - 시니어는 손상 CSV 정책과 전역 상태를 구조적 리스크로 크게 봤다.
  - 주니어와 초심자는 당장 기능 실패보다 “설명 부족”을 더 큰 문제로 봤다.
- 우선순위가 높은 개선 후보
  - benchmark 결과 해석 문구 보강
  - 손상 CSV 처리 정책 통일
  - `row offset`, `cold/warm` 설명 보강

# 다음 작업

- README 또는 발표 자료에 아래 문장을 명시한다.
  - `cold`는 첫 PK 조회 시 인덱스 재구성 비용을 포함한다.
  - `warm`은 이미 메모리에 만든 인덱스를 재사용한 조회다.
  - benchmark 숫자는 현재 구현에서 `clock()` 기준이다.
- 손상 CSV를 어떻게 다룰지 정책을 정한다.
  - 빈 파일을 오류로 볼지
  - 빈 테이블로 볼지
  - 모든 경로에서 같은 방식으로 처리할지
- 필요하면 `row offset -> ftell()/fseek()` 설명 예시를 README에 추가한다.

# 남은 리스크

- benchmark 수치를 wall-clock latency처럼 발표하면 오해가 생길 수 있다.
- 현재 benchmark는 데이터 생성, 인덱스 재구성, 조회 비용이 한 표 안에서 함께 해석될 여지가 있다.
- 손상 CSV 처리 정책이 일관되지 않아, 실제 사용자 환경에서 SELECT와 INSERT의 반응이 다르게 보일 수 있다.
- `table_states`는 프로세스 생애주기 전체에 남는 전역 상태이므로,
  장기적으로는 초기화/해제 전략을 고민할 필요가 있다.

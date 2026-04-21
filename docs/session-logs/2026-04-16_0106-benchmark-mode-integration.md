# 요청 요약

- `sqlproc`와 분리돼 있던 benchmark 흐름을 `-b`, `--benchmark` 인자로 통합한다.
- benchmark가 더미 데이터 개수를 직접 묻고, PK / non-PK 비교 표를 보여 준 뒤
  일반 interactive 모드로 이어지게 만든다.
- 관련 문서와 `make bench` 진입 경로도 새 흐름에 맞춰 정리한다.

# 결정 사항

- benchmark 산출물은 원래 `data_dir`를 오염시키지 않도록 `<data_dir>/benchmark/`
  아래에 따로 만든다.
- PK 비교는 cold / warm를 둘 다 보여 주고, non-PK 비교는 `name` equality scan으로
  고정한다.
- benchmark SQL도 별도 우회 로직이 아니라 `run_sql_file()` 공용 경로로 실행한다.
- benchmark가 성공적으로 끝나면 바로 interactive 모드로 넘어간다.

# 현재 브랜치 상태

- 브랜치: `feat/benchmark-mode`
- 최근 커밋:
  - `6118160` `test: 벤치마크 모드와 복귀 흐름 검증을 추가`
  - `f960163` `feat: sqlproc에 벤치마크 실행 모드를 추가`

# 완료한 작업

- `AppConfig`에 `benchmark_mode`를 추가하고 인자 파싱이 `-b`, `--benchmark`,
  `--interactive`, 파일 실행을 함께 해석하도록 바꿨다.
- `run_sql_file()` 공용 실행 함수를 추출해 일반 파일 실행과 benchmark가 같은
  파이프라인을 타도록 정리했다.
- `src/benchmark.c`를 추가해 더미 CSV, benchmark SQL 2개, 출력 캡처, 시간 측정을
  구현했다.
- benchmark 종료 뒤 원래 `schema_dir`, `data_dir` 기준 interactive 모드로
  자연스럽게 복귀하도록 연결했다.
- 자동 테스트에 benchmark 입력 주입, 재프롬프트, 결과 표, interactive 복귀,
  benchmark 하위 파일 생성, 원본 `data/users.csv` 비오염 검증을 추가했다.
- `README.md`, `GUIDE.md`, `AGENTS.md`를 새 benchmark 진입 경로 기준으로 갱신했다.

# 리뷰 결과

- 정확성/버그: benchmark는 실제 `sqlproc` 파일 실행 경로를 재사용하고, PK cold와
  warm 측정이 서로 다른 data_dir cache key와 충돌하지 않음을 확인했다.
- 자료구조 무결성: benchmark 전용 데이터는 `<data_dir>/benchmark/` 아래 별도 경로를
  사용해 원래 interactive 대상 데이터와 섞이지 않게 유지했다.
- 초심자 가독성/C99 난이도: `benchmark.c`에 경로 준비, 입력 파싱, silent 실행을
  작은 함수로 나눠 흐름을 읽기 쉽게 유지했다.

# 다음 작업

- 팀 시연에서 사용할 benchmark record count와 발표용 표 형식을 실제 발표 환경에서
  한 번 더 맞춘다.
- 필요하면 README의 예시 수치를 팀 노트북 실측값으로 다시 갱신한다.

# 남은 리스크

- benchmark 수치는 머신 성능과 현재 파일 시스템 상태에 따라 달라진다.
- `make bench`는 사용자 입력을 기다리므로 비대화형 환경에서는 별도 입력 주입이
  필요하다.

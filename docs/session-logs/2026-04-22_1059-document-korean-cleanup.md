# 문서 한국어 정리

## 요청 요약

- `/document-release` 흐름으로 한국어 문서를 현재 구현 상태에 맞게 정리한다.
- `main`에서 직접 수정하지 않고 문서 정리용 기능 브랜치를 만든 뒤 진행한다.

## 결정 사항

- 브랜치: `feature/document-korean-cleanup`
- 기준 브랜치: `refs/heads/main`
- 현재 구현은 B+ Tree, PK 자동 증가, `WHERE =/>/</!=`, `[INDEX]`, `[INDEX-RANGE]`, `[SCAN]`, benchmark 모드를 이미 포함하므로 문서를 계획형 표현에서 구현 완료 표현으로 바꾼다.
- 원격 이름도 `main`이라 `git diff main...HEAD`가 ambiguous 경고를 낼 수 있어, 비교 기준은 `refs/heads/main`으로 명시한다.

## 현재 브랜치 상태

- `feature/document-korean-cleanup`
- 작업 시작 시 `build-asan/` 미추적 디렉터리가 이미 있었다.
- 검증 중 생성된 `demo-data/benchmark/` 산출물은 정리했다.

## 완료한 작업

- `README.md` 제목을 `week7-02-sql-index`로 정리하고 관련 문서 목록을 추가했다.
- `AGENTS.md`의 프로젝트명, 저장소 정보, 주요 파일 경로, 지원 SQL, B+ Tree 구현 상태를 현재 저장소 기준으로 갱신했다.
- `AGENTS_7.md`, `PLAN_7.md`를 구현 예정 문서에서 구현 상태/기록 문서로 정리했다.
- `GUIDE.md`에 `WHERE` 지원 범위, B+ Tree 모듈, benchmark 모드, 현재 storage/executor 흐름을 반영했다.
- `docs/storage-executor.md`에 executor, storage, bptree 세 모듈의 연결 흐름과 offset 기반 조회 함수를 추가했다.
- INSERT/SELECT 컬럼 순서 설명 문서의 오래된 절대 경로를 현재 저장소 경로로 수정했다.
- `docs/bptree-easy-guide.md`의 테스트 라인 링크를 현재 코드 위치에 맞췄다.
- `docs/images/demo-run.svg`의 프로젝트명을 현재 저장소명으로 수정했다.

## 리뷰 결과

- 오래된 다운로드 디렉터리 기반 절대 경로를 제거했다.
- 예전 프로젝트명, `WHERE` 미지원 표현, B+ Tree 예정 표현, 오래된 storage helper 중심 설명을 현재 구현과 맞췄다.
- README에서 주요 문서를 모두 연결해 문서 discoverability를 보강했다.

## 검증

- `make`
  - 결과: 성공, 이미 최신 빌드라 `Nothing to be done for all`
- `make test`
  - 결과: 성공, `All tests passed.`
- `printf '1000\n.exit\n' | ./build/sqlproc --schema-dir ./examples/schemas --data-dir ./demo-data --benchmark`
  - 결과: 성공, benchmark 결과 표 출력 후 interactive 프롬프트로 복귀
- `git diff --check`
  - 결과: 성공
- stale 문구 검색
  - 예전 절대 경로, 예전 프로젝트명, 오래된 `WHERE` 미지원 표현, 오래된 storage helper 설명 검색 결과 없음

## 다음 작업

- 문서 변경만 stage 후 커밋한다.
- 필요하면 PR 본문에 문서 변경 요약을 추가한다.

## 남은 리스크

- `build-asan/`은 이번 작업 전부터 있던 미추적 디렉터리라 그대로 둔다.
- benchmark 수치는 입력 개수와 실행 환경에 따라 달라진다.

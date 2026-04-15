# 요청 요약

- `src` 폴더의 모든 `.c` 파일에 함수별 한국어 주석을 보강한다.
- 각 함수 주석에는 기능, 입력, 출력 정보를 포함한다.
- 초심자가 막히기 쉬운 일부 내부 코드에는 한국어 보조 설명을 추가한다.
- 작업 후 `three-persona-multi-agent-review` 방식으로 주석 품질을 검토한다.

# 결정 사항

- 함수 헤더 주석은 전 파일에 통일된 형식으로 추가한다.
- 내부 주석은 포인터 이동, B+ Tree 분할, CSV 파싱, row offset 계산처럼
  초심자에게 특히 어려운 지점에만 선별적으로 추가한다.
- 저장소 규칙에 맞추기 위해 `feature/src-comment-annotations` 브랜치에서 작업한다.

# 현재 브랜치 상태

- 현재 브랜치: `feature/src-comment-annotations`
- 작업 시작 시 참고 상태:
  `## feature/src-comment-annotations`
- 사용자 작업으로 보이는 별도 변경:
  `.gitignore`, `build-asan/`, `data/benchmark/`

# 완료한 작업

- `src/app.c`, `src/main.c`, `src/schema.c`, `src/tokenizer.c`,
  `src/parser.c`, `src/bptree.c`, `src/executor.c`, `src/storage.c`
  에 함수별 한국어 주석을 추가했다.
- `src/bptree.c`에 leaf/internal split 설명을 보강했다.
- `src/executor.c`에 인덱스 선택 분기와 range scan 결과 버퍼 확장 의도를
  설명하는 주석을 추가했다.
- `src/storage.c`에 CSV 이스케이프 처리와 `ftell` 기반 row offset 계산 설명을
  추가했다.
- `make`, `make test`를 실행해 빌드와 테스트 통과를 확인했다.

# 리뷰 결과

- `three-persona-multi-agent-review`로 세 관점 리뷰를 수행했다.
- 공통 평가:
  주석 방향은 좋고, CSV 처리/인덱스 분기/B+ Tree split 설명이 특히 도움이 된다는
  피드백을 받았다.
- 공통 보완 요청:
  `schema.c`의 `id:int` 자동 PK 규칙,
  `executor.c`의 PK `=`, `>`, `<` 인덱스 사용 조건,
  `bptree.c`의 핵심 용어(`leaf`, `internal`, `promoted_key`, `next`)를
  더 직접적으로 설명하면 좋겠다는 의견이 모였다.
- 반영 내용:
  위 세 항목을 코드 주석에 추가 또는 수정했다.

# 다음 작업

- 커밋 메시지를 규칙에 맞춰 작성하고 커밋한다.

# 남은 리스크

- 이번 변경은 주석 중심이므로 동작 변화는 없지만, 작은 헬퍼 함수까지 같은 형식의
  헤더 주석이 들어가 스캔성이 조금 떨어질 수 있다는 의견이 있었다.
- 다만 사용자 요청이 "모든 함수별 기능/입력/출력 주석"이었기 때문에 이번 범위에서는
  일관성을 우선 유지했다.

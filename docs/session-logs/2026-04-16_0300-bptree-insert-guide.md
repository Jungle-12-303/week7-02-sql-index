# 요청 요약

- `docs/bptree-easy-guide.md`의 삽입 설명을 초심자 기준으로 더 보강해 달라는 요청을 받았다.
- 특히 아래 항목을 더 풀어 설명하는 것이 목표였다.
  - promoted key가 어떤 기준으로 정해지는지
  - leaf split과 internal split이 어떻게 진행되는지
  - split이 부모와 root까지 어떻게 전파되는지
- 수정 후에는 `$three-persona-multi-agent-review`로 검토하고,
  `$commit-convention` 규칙에 맞춰 커밋하기로 했다.

# 결정 사항

- 기존 leaf split 예시만 보강하는 수준을 넘어서,
  "삽입 전 임시 배열 생성 -> split_index 계산 -> promoted key 선정 ->
  parent/root 전파" 흐름을 한 섹션 안에서 끊기지 않게 설명한다.
- leaf split과 internal split에서 promoted key가 다르게 정해지는 이유를
  비교 표로 정리한다.
- 초심자 관점에서 혼동이 잦은 용어는 아래처럼 명시한다.
  - `promoted key`: 부모로 올라가는 경계값
  - `split_index`: 삽입이 반영된 임시 배열 기준으로 계산되는 분할 위치

# 현재 브랜치 상태

- 작업 브랜치: `changwon/codex`
- 작업 시작 시 `git status --short --branch` 결과:
  `## changwon/codex...main/changwon/codex [ahead 2]`
- 기존 미추적 경로 `data/benchmark/`는 이번 작업과 무관하여 그대로 유지했다.

# 완료한 작업

- [docs/bptree-easy-guide.md](/Users/donghyunkim/Documents/week7-02-sql-index/docs/bptree-easy-guide.md)에
  아래 내용을 추가했다.
  - split 전에 `temp_keys[]`, `temp_offsets[]`, `temp_children[]`를 만드는 이유
  - `split_index = total_count / 2`, `split_index = total_keys / 2`의 의미
  - leaf split에서 `right->keys[0]`이 promoted key가 되는 이유
  - internal split에서 가운데 key가 부모로 올라가고 좌우 node에서는 빠지는 이유
  - leaf split과 internal split의 promoted key 차이 비교 표
  - root split이 leaf/internal split 결과의 전파로 생긴다는 설명
- internal split 예시에 child가 담당하는 key 범위도 넣어,
  `c0`~`c4`가 무엇을 뜻하는지 바로 읽히도록 했다.
- 새 root는 처음 생성될 때 split된 두 child를 가리켜 자식 2개로 시작한다는 점도 적었다.

# 리뷰 결과

## 시니어 프로그래머(코치)

- 문서 설명은 현재 `src/bptree.c`의 leaf/internal/root split 흐름과 정확히 맞는다고 평가했다.
- 보강 포인트로는 아래 두 가지를 제안했다.
  - `split_index`가 현재 node가 아니라 삽입 후 임시 배열 기준이라는 점을 더 명확히 쓰기
  - leaf에서의 promoted key 설명이 현재 프로젝트의 `BPTREE_ORDER = 4` 구현 기준임을 더 또렷하게 쓰기

## 주니어 프로그래머(서포터)

- `split 전에 먼저 하는 일 -> leaf split -> internal split -> root split` 흐름이 자연스럽다고 평가했다.
- `temp_*` 배열을 먼저 만든다는 설명이 특히 좋다고 봤다.
- 다만 설명이 약간 길어질 수 있으니, 비교 표와 핵심 정의를 중심으로 정리하면 좋겠다고 했다.

## C 초심자(학생)

- leaf split과 internal split을 나눠 설명한 점이 이전보다 훨씬 읽기 쉽다고 평가했다.
- 다만 아래 부분은 한 번 더 짚어주면 좋겠다고 했다.
  - `promoted key`의 한 줄 정의
  - `split_index`가 임시 배열 기준이라는 점
  - internal 예시에서 child가 맡는 key 범위
  - 새 root가 처음에는 자식 2개로 시작한다는 점

## 리뷰 반영 결과

- 세 페르소나가 공통으로 요청한 보강을 문서에 반영했다.
  - `promoted key` 한 줄 정의 추가
  - `split_index` 설명을 임시 배열 기준으로 보강
  - internal child 범위 예시 추가
  - 새 root의 초기 자식 수 설명 추가

# 검증 결과

- `git diff --check` 통과
- `make test` 통과
  - 결과: `All tests passed.`

# 다음 작업

- 필요하면 발표용으로 `leaf split -> parent split -> new root`를 실제 key 삽입 순서로
  한 장짜리 슬라이드용 Mermaid로 더 압축할 수 있다.
- `test_bptree_multiple_keys_and_split()`가 어떤 삽입 순서를 쓰는지 문서에 연결하면
  학습용 자료로 더 좋아질 수 있다.

# 남은 리스크

- 문서 설명은 현재 구현과 맞지만, 다른 B+ Tree 변형이나 다른 order 설정까지
  보편적으로 설명하는 문서는 아니다.
- `data/benchmark/`는 여전히 미추적 상태이므로, benchmark sample을 저장소 기준 자료로
  완전히 고정하려면 별도 합의가 필요하다.

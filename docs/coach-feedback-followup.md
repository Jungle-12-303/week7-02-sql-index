# 시니어 코치 질문 후속 정리

> 목적: 실제 시니어 코치님의 질문 의도를 현재 코드베이스에 연결해 해석하고,
> 발표/QnA/다음 리팩터링에서 바로 사용할 수 있는 답변과 개선 방향을 정리한다.

---

## 문서 범위

이 문서는 아래 코드와 직접 연결해 해석한다.

- [include/bptree.h](/Users/donghyunkim/Documents/week7-02-sql-index/include/bptree.h)
- [src/bptree.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/bptree.c)
- [src/executor.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/executor.c)
- [src/storage.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/storage.c)
- [benchmarks/bench_index.c](/Users/donghyunkim/Documents/week7-02-sql-index/benchmarks/bench_index.c)

특히 아래 구현이 핵심 전제다.

- 인덱스는 `id -> CSV row offset`만 저장한다.
- exact lookup은 `bptree_search()`를 사용한다.
- range lookup은 leaf의 `next` 포인터를 따라가며 offset 목록을 만든 뒤 출력한다.
- 프로그램 재시작 시 인덱스는 사라지므로 `storage_rebuild_pk_index()`로 다시 만든다.

---

## 1. Big O만으로는 왜 충분하지 않은가

### 질문 의도

코치님의 첫 질문은 "B+ Tree도, B-Tree도, 균형 잡힌 이진 탐색 트리도 검색 Big O는 결국 `O(log n)`인데, 왜 실제 성능 차이가 크게 나는가?"를 묻는 질문이다.

즉, 자료구조를 설명할 때 `O(log n)` 하나로 끝내지 말고 아래까지 설명할 수 있어야 한다는 뜻이다.

- 트리 높이
- 노드 하나를 읽을 때 드는 실제 비용
- 메모리/캐시 친화성
- 디스크/파일 I/O 패턴
- 범위 조회를 할 때의 순차 접근 가능성

### 현재 코드에서 보이는 지점

현재 구현은 `BPTREE_ORDER 4` 고정이다. [include/bptree.h](/Users/donghyunkim/Documents/week7-02-sql-index/include/bptree.h:12)

즉 한 노드의 fan-out이 작고, 그만큼 높이가 커지기 쉽다.

```c
#define BPTREE_ORDER 4
#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)
```

또한 노드 내부 탐색도 이진 탐색이 아니라 선형 탐색이다. [src/bptree.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/bptree.c:91) [src/bptree.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/bptree.c:103)

```c
while (position < node->key_count && node->keys[position] < key) {
    position += 1;
}
```

그래서 현재 구현의 실제 비용은 단순히 "검색은 `O(log n)`"이라고만 설명하면 부족하다.

### 현재 코드 기준 답변

현재 프로젝트에서 B+ Tree가 유리한 이유는 Big O 자체보다는 다음에 더 가깝다.

1. PK exact lookup은 CSV 전체를 읽는 `O(n)` scan 대신, 트리를 타고 바로 offset 하나를 찾는다.
2. range lookup은 leaf의 `next` 연결 덕분에 순차적으로 결과를 모을 수 있다.
3. 반면 비PK 조건은 인덱스가 없어서 여전히 CSV 전체 scan이다.

즉 이번 발표에서는 "B+ Tree의 Big O가 특별해서 빠르다"보다 "CSV 전체를 다 읽지 않고 필요한 위치를 바로 찾기 때문에 빠르다"라고 설명하는 편이 더 정확하다.

### 향후 개선 방안

- README나 발표에서 Big O 표와 함께 "실제 성능을 가르는 요소"를 별도 표로 정리한다.
- `BPTREE_ORDER`를 바꿔 fan-out 변화에 따른 높이와 성능을 비교한다.
- 노드 내부 탐색을 선형 탐색 그대로 둘지, 차수가 커질 경우 이진 탐색으로 바꿀지 검토한다.

### 발표용 한 줄 요약

`O(log n)`이라는 표기만 보면 비슷해 보여도, 실제로는 fan-out, 트리 높이, 캐시/파일 접근 패턴 때문에 B+ Tree가 더 유리해질 수 있습니다.

---

## 2. CSV를 row 단위로 읽는 대신 더 크게 묶어 읽을 수 없는가

### 질문 의도

이 질문은 "현재 인덱스 자체보다 CSV I/O가 병목일 수 있는데, 그 부분을 너무 당연하게 넘기고 있지 않나?"를 짚는 질문이다.

특히 이 코드베이스에서는 다음 흐름이 있다.

- 인덱스 재구성 시 CSV 전체를 `fgets()`로 한 줄씩 읽는다.
- range 결과 출력 시 offset마다 `fseek()` + `fgets()`를 반복한다.

즉 자료구조보다 I/O 패턴이 더 큰 비용이 될 수 있다.

### 현재 코드에서 보이는 지점

인덱스 재구성은 `storage_rebuild_pk_index()`에서 row 단위로 이뤄진다. [src/storage.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/storage.c:1127)

```c
if (fgets(line, sizeof(line), file) == NULL) {
    break;
}
```

range 출력은 `storage_print_rows_at_offsets()`에서 offset 목록을 돌며 각 row마다 `fseek()` + `fgets()`를 수행한다. [src/storage.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/storage.c:840) [src/storage.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/storage.c:791)

```c
for (i = 0; i < offset_count; i++) {
    if (!print_row_from_open_file(file, schema, offsets[i], ...)) {
        ...
    }
}
```

```c
if (fseek(file, offset, SEEK_SET) != 0) {
    ...
}

if (fgets(line, sizeof(line), file) == NULL) {
    ...
}
```

### 현재 코드 기준 답변

"묶어서 한 번에 읽는 방식"은 분명 개선 여지가 있다. 다만 CSV는 가변 길이 row이기 때문에, 단순 배열처럼 "인덱스 * 고정 row 크기"로 바로 계산할 수는 없다.

그래서 현재 구현은 가장 단순하고 안전한 방법을 택했다.

- 재구성: 처음부터 끝까지 한 줄씩 읽으며 `id`와 `offset`을 등록
- 조회: offset으로 이동한 뒤 해당 row 한 줄만 읽음

이 방식은 구현과 디버깅은 쉽지만, 대량 데이터에서 I/O 호출 수가 많아질 수 있다.

### 향후 개선 방안

우선순위 순으로 보면 아래가 현실적이다.

1. `fread()` 기반 chunk scanner
   - 큰 버퍼로 파일을 읽고, 메모리에서 개행 기준으로 row를 분리한다.
   - `fgets()` 호출 수를 줄일 수 있다.

2. `mmap()` 기반 읽기
   - 파일 전체를 메모리 맵으로 붙여 놓고 row boundary를 직접 탐색한다.
   - 구현 난이도는 올라가지만 재구성 속도는 좋아질 수 있다.

3. CSV 대신 바이너리 row store 또는 고정 길이 포맷
   - 이번 주 과제 범위를 넘지만, offset 기반 접근의 진짜 장점은 이런 포맷에서 더 크게 살아난다.

4. range 출력 시 "offset 목록 생성"과 "row 읽기"를 분리하지 않고 스트리밍 처리
   - 지금은 offset을 모두 모은 뒤 다시 파일을 읽는다.
   - leaf traversal 중 바로 출력하는 방식으로 메모리 사용을 줄일 수 있다.

### 발표용 한 줄 요약

현재는 구현 단순성을 위해 row 단위 `fgets()`를 선택했지만, 대량 데이터에서는 chunk read나 `mmap()`처럼 I/O 호출 수를 줄이는 방향이 더 빠를 수 있습니다.

---

## 3. offset 목록을 free하면 비슷한 조건에서 손해 아닌가

### 질문 의도

이 질문의 핵심은 "현재 구현이 쿼리 결과를 일회용으로만 보고 있는데, 반복되는 비슷한 질의 workload를 고려하면 재활용 여지가 있지 않나?"이다.

즉, 아래 둘을 구분해서 생각해 보라는 뜻이다.

- 인덱스 자체의 캐시
- 질의 결과(offset 목록)의 캐시

현재 코드는 첫 번째는 있다. 두 번째는 없다.

### 현재 코드에서 보이는 지점

range query는 `OffsetList`에 결과 offset을 모은 뒤 출력하고, 바로 `free()`한다. [src/executor.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/executor.c:601) [src/executor.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/executor.c:705)

```c
typedef struct {
    long *offsets;
    int count;
    int capacity;
} OffsetList;
```

```c
ok = storage_print_rows_at_offsets(..., offsets.offsets, offsets.count, ...);
free(offsets.offsets);
```

즉 `id < 20`을 한 번 실행하고 곧바로 `id < 18`을 실행해도, 두 번째 질의는 첫 번째 결과를 재활용하지 않는다.

### 현재 코드 기준 답변

맞다. 현재 구현은 질의 결과를 재사용하지 않는다.

그 대신 얻는 장점은 아래와 같다.

- 캐시 무효화 문제가 없다.
- INSERT 후 stale result를 걱정하지 않아도 된다.
- 코드가 단순하다.

반대로 손해도 분명하다.

- 비슷한 range query가 반복되면 매번 leaf traversal과 offset 수집을 다시 해야 한다.
- offset 목록을 메모리에 모았다가 버리므로, 같은 패턴의 질의에서 중복 비용이 생긴다.

즉 현재 구조는 "학습용/단순성 우선"이고, 반복 질의 최적화는 아직 넣지 않은 상태라고 답하는 것이 정확하다.

### 향후 개선 방안

아래 중 현실적인 순서로 검토할 수 있다.

1. 최근 질의 결과 캐시
   - 예: `WHERE id < 20` 결과를 최근 캐시에 저장
   - 다음 `WHERE id < 18`은 prefix만 잘라 재사용 가능
   - 단, INSERT가 발생하면 캐시 무효화 정책 필요

2. offset 목록 대신 leaf cursor 재사용
   - 최근 range 시작 leaf와 경계 key를 저장
   - 비슷한 범위 질의에서 더 빠르게 재시작 가능

3. range query 결과를 즉시 출력하는 streaming 방식
   - 현재는 "수집 후 출력"이다.
   - "순회하며 즉시 출력"으로 바꾸면 offset 배열 할당/해제를 줄일 수 있다.

4. query result cache보다 먼저 할 수 있는 더 값싼 개선
   - `id > x and id < y` 같은 범위는 offset 배열 생성 없이 visitor 기반 출력
   - 최근 exact lookup 결과 한두 개 캐시

### 그 외 성능 개선 방법

현재 코드베이스 기준으로 효과 대비 난이도를 따지면 아래 순서가 좋다.

1. `BPTREE_ORDER` 튜닝
2. range query의 streaming 출력
3. 재구성/조회 I/O를 chunk read로 변경
4. 최근 질의 결과 캐시 추가
5. CSV 대신 더 index-friendly한 저장 포맷 검토

### 발표용 한 줄 요약

현재는 결과 offset을 일회용으로 사용해 코드를 단순하게 유지했고, 반복 질의 최적화는 캐시 무효화 비용 때문에 아직 의도적으로 넣지 않았습니다.

---

## 4. B+ Tree의 적절한 차수는 어떻게 구할 것인가

### 질문 의도

이 질문은 "차수 4가 그냥 예제값인지, 어떤 근거로 정한 값인지 설명할 수 있나?"를 묻는 질문이다.

좋은 답변은 보통 아래 둘을 같이 포함해야 한다.

- 이론적 기준
- 현재 구현에 맞는 실용적 기준

### 현재 코드에서 보이는 지점

현재는 차수를 상수로 고정했다. [include/bptree.h](/Users/donghyunkim/Documents/week7-02-sql-index/include/bptree.h:12)

```c
#define BPTREE_ORDER 4
```

노드 구조는 `key` 배열과 `children` 또는 `offsets` 배열을 모두 정적으로 가진다. [src/bptree.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/bptree.c:10) [src/bptree.c](/Users/donghyunkim/Documents/week7-02-sql-index/src/bptree.c:24)

즉 차수가 커질수록:

- 한 노드당 메모리 사용량은 늘고
- split은 줄고
- 트리 높이는 낮아지고
- 노드 내부 선형 탐색 비용은 커진다

### 현재 코드 기준 답변

현재 `4`는 "설명과 검증이 쉬운 값"으로는 좋지만, "성능 최적화된 값"이라고 보긴 어렵다.

이 코드베이스는 메모리 기반 B+ Tree이므로, 디스크 페이지보다 CPU cache와 노드 크기를 더 의식하는 편이 맞다.

차수를 정할 때는 보통 아래 기준을 본다.

1. 노드 하나가 너무 커지지 않는가
2. fan-out 증가로 트리 높이가 충분히 줄어드는가
3. 노드 내부 탐색 비용이 과하게 커지지 않는가
4. 실제 workload에서 benchmark 결과가 좋은가

즉 "적절한 차수"는 공식 하나로 끝내기보다, 노드 크기 추정 + 실제 benchmark를 함께 봐야 한다.

### 추천 접근

현재 구조에서는 아래 접근이 가장 현실적이다.

1. `BPTREE_ORDER`를 compile-time configurable하게 바꾼다.
   - 예: `cc -DBPTREE_ORDER=8`
   - 헤더에서 기본값만 제공

2. `bench_index`로 차수별 비교를 돌린다.
   - 후보: `4`, `8`, `16`, `32`
   - 측정 항목:
     - 100만 건 삽입 시간
     - exact lookup 시간
     - range lookup 시간
     - 메모리 사용량

3. 현재 구현이 노드 내부를 선형 탐색하므로 너무 큰 차수는 피한다.
   - 이 구현에서는 `16` 또는 `32`가 더 나은 출발점일 가능성이 높다.
   - `4`는 split 설명용으로는 좋지만 성능용으로는 작다.

### 간단한 근거식

메모리 기반에서는 대략 "노드 하나가 cache line 몇 개를 먹는지"를 기준으로 잡을 수 있다.

현재 노드에서 차수 `m`일 때 대략 필요한 공간은 아래처럼 증가한다.

- key 배열: `4 * (m - 1)` bytes
- child 배열 또는 offset 배열: 대략 `8 * m` bytes
- 메타데이터와 정렬 패딩: 추가

즉 차수가 커질수록 fan-out은 좋아지지만, 한 노드를 읽고 선형 탐색하는 비용도 함께 커진다.

### 발표용 한 줄 요약

현재 차수 4는 학습과 split 검증에는 좋지만, 메모리 기반 성능 최적화를 생각하면 8~32 범위를 benchmark로 비교해 결정하는 편이 더 타당합니다.

---

## 최종 정리

코치님의 질문은 단순히 "더 빠르게 할 수 있나?"가 아니라, 아래를 설명할 수 있는지 점검하는 질문이다.

1. Big O와 실제 성능을 구분해서 말할 수 있는가
2. 인덱스뿐 아니라 CSV I/O도 병목이라는 점을 보고 있는가
3. 현재 구조가 단순성을 위해 포기한 최적화가 무엇인지 알고 있는가
4. 차수 같은 상수를 "왜 그렇게 정했는지" 근거를 만들 수 있는가

현재 코드베이스는 과제 요구사항을 만족하는 교육용 구현으로는 충분히 설득력 있다. 다만 성능과 확장성까지 더 밀어붙이려면 다음 순서가 좋다.

1. `BPTREE_ORDER`를 실험 가능한 값으로 바꾸기
2. range query를 offset 배열 없이 streaming 처리하기
3. CSV 재구성과 조회를 chunk read 또는 `mmap()`으로 개선하기
4. 반복 질의를 위한 result/cache 전략을 선택적으로 도입하기

---

## 발표에서 바로 말할 수 있는 짧은 답변

> 현재 구현은 과제 요구사항을 우선 만족하도록 단순성과 설명 가능성을 택했습니다.  
> 그래서 B+ Tree는 `id -> offset`만 저장하고, CSV는 줄 단위로 읽으며, range 결과도 일회용 offset 리스트로 처리합니다.  
> 다만 성능을 더 밀어붙이려면 차수 튜닝, range streaming, chunk read, 결과 캐시 같은 개선 여지가 분명히 있습니다.  
> 즉 지금 구조는 "맞게 동작하는 1차 구현"이고, 코치님 질문은 그 다음 단계 최적화 방향을 짚어주신 것으로 이해하고 있습니다.

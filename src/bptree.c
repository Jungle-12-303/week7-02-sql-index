#include <stdlib.h>
#include <string.h>

#include "bptree.h"

#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

/*
 * B+ Tree 용어를 아주 짧게 정리하면:
 * - leaf: 실제 key -> row offset 데이터를 저장하는 맨 아래 노드
 * - internal: 어느 자식으로 내려갈지 안내하는 경계 key를 저장하는 노드
 * - promoted_key: split 뒤 부모에게 "경계선"으로 올려 보내는 key
 * - next: leaf끼리 오른쪽 이웃을 잇는 포인터로, range scan에 사용
 */

typedef struct BPlusTreeNode BPlusTreeNode;

typedef struct {
    long offsets[BPTREE_MAX_KEYS];
    BPlusTreeNode *next;
} BPlusTreeLeafData;

typedef struct {
    BPlusTreeNode *children[BPTREE_ORDER];
} BPlusTreeInternalData;

typedef union {
    BPlusTreeLeafData leaf;
    BPlusTreeInternalData internal;
} BPlusTreeNodeData;

struct BPlusTreeNode {
    int is_leaf;
    int key_count;
    int keys[BPTREE_MAX_KEYS];
    BPlusTreeNodeData data;
};

struct BPlusTree {
    BPlusTreeNode *root;
};

typedef struct {
    int split;
    int duplicate;
    int failed;
    int promoted_key;
    BPlusTreeNode *right_node;
} InsertResult;

/* B+ Tree의 leaf 또는 internal 노드 1개를 동적 할당한다.
 *
 * 입력:
 * - is_leaf: leaf 노드면 1, internal 노드면 0
 * 출력:
 * - 반환값: 생성된 노드 포인터, 메모리 부족이면 NULL
 */
static BPlusTreeNode *create_node(int is_leaf)
{
    BPlusTreeNode *node;

    node = (BPlusTreeNode *)calloc(1, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }

    node->is_leaf = is_leaf;
    return node;
}

/* 노드와 그 하위 서브트리를 재귀적으로 해제한다.
 *
 * 입력:
 * - node: 해제할 노드 포인터, NULL 허용
 * 출력:
 * - 반환값 없음
 * - 부가 효과: internal 노드는 모든 자식까지 함께 free됨
 */
static void destroy_node(BPlusTreeNode *node)
{
    int i;

    if (node == NULL) {
        return;
    }

    if (!node->is_leaf) {
        for (i = 0; i <= node->key_count; i++) {
            destroy_node(node->data.internal.children[i]);
        }
    }

    free(node);
}

/* 비어 있는 B+ Tree 핸들을 생성한다.
 *
 * 입력:
 * - 없음
 * 출력:
 * - 반환값: 새 트리 포인터, 메모리 부족이면 NULL
 */
BPlusTree *bptree_create(void)
{
    BPlusTree *tree;

    tree = (BPlusTree *)calloc(1, sizeof(*tree));
    return tree;
}

/* B+ Tree 전체를 해제한다.
 *
 * 입력:
 * - tree: 해제할 트리 포인터, NULL 허용
 * 출력:
 * - 반환값 없음
 * - 부가 효과: 모든 노드 메모리가 반환됨
 */
void bptree_destroy(BPlusTree *tree)
{
    if (tree == NULL) {
        return;
    }

    destroy_node(tree->root);
    free(tree);
}

/* leaf 노드에서 key가 들어갈 위치 또는 같은 key 위치를 찾는다.
 *
 * 입력:
 * - node: leaf 노드 포인터
 * - key: 찾을 키 값
 * 출력:
 * - 반환값: node->keys 배열 안의 삽입/검색 위치 인덱스
 */
static int find_leaf_position(const BPlusTreeNode *node, int key)
{
    int position;

    position = 0;
    while (position < node->key_count && node->keys[position] < key) {
        position += 1;
    }

    return position;
}

/* internal 노드에서 다음으로 내려갈 자식 인덱스를 계산한다.
 *
 * 입력:
 * - node: internal 노드 포인터
 * - key: 찾거나 삽입할 키 값
 * 출력:
 * - 반환값: 내려갈 child 배열 인덱스
 */
static int find_child_position(const BPlusTreeNode *node, int key)
{
    int position;

    position = 0;
    while (position < node->key_count && key >= node->keys[position]) {
        position += 1;
    }

    return position;
}

/* 여유가 있는 leaf 노드에 key-offset 쌍을 직접 삽입한다.
 *
 * 입력:
 * - leaf: 삽입 대상 leaf 노드
 * - position: 삽입할 배열 위치
 * - key: 저장할 키 값
 * - offset: key에 대응하는 CSV row offset
 * 출력:
 * - 반환값 없음
 * - leaf: 키와 오프셋이 삽입되고 key_count가 1 증가함
 */
static void insert_leaf_without_split(BPlusTreeNode *leaf,
                                      int position,
                                      int key,
                                      long offset)
{
    int i;

    for (i = leaf->key_count; i > position; i--) {
        leaf->keys[i] = leaf->keys[i - 1];
        leaf->data.leaf.offsets[i] = leaf->data.leaf.offsets[i - 1];
    }

    leaf->keys[position] = key;
    leaf->data.leaf.offsets[position] = offset;
    leaf->key_count += 1;
}

/* 가득 찬 leaf 노드를 둘로 나누면서 새 key-offset 쌍을 삽입한다.
 *
 * 입력:
 * - leaf: 분할할 원래 leaf 노드
 * - key: 새로 삽입할 키 값
 * - offset: key에 대응하는 CSV row offset
 * 출력:
 * - 반환값: split 여부, promoted_key, 새 오른쪽 노드를 담은 결과 구조체
 * - leaf/right_node: 성공 시 두 leaf로 재배치되고 next 연결이 갱신됨
 */
static InsertResult split_leaf_and_insert(BPlusTreeNode *leaf,
                                          int key,
                                          long offset)
{
    InsertResult result;
    BPlusTreeNode *right;
    int temp_keys[BPTREE_MAX_KEYS + 1];
    long temp_offsets[BPTREE_MAX_KEYS + 1];
    int insert_position;
    int total_count;
    int split_index;
    int i;

    memset(&result, 0, sizeof(result));
    right = create_node(1);
    if (right == NULL) {
        result.failed = 1;
        return result;
    }

    insert_position = find_leaf_position(leaf, key);

    for (i = 0; i < insert_position; i++) {
        temp_keys[i] = leaf->keys[i];
        temp_offsets[i] = leaf->data.leaf.offsets[i];
    }

    temp_keys[insert_position] = key;
    temp_offsets[insert_position] = offset;

    for (i = insert_position; i < leaf->key_count; i++) {
        temp_keys[i + 1] = leaf->keys[i];
        temp_offsets[i + 1] = leaf->data.leaf.offsets[i];
    }

    /*
     * leaf split은 "실제 데이터"를 왼쪽/오른쪽 leaf에 다시 나눠 담는 과정입니다.
     * 이때 부모는 오른쪽 leaf가 어디서 시작되는지만 알면 되므로,
     * 오른쪽 leaf의 첫 key를 promoted_key로 올려 경계선처럼 사용합니다.
     */
    total_count = leaf->key_count + 1;
    split_index = total_count / 2;

    leaf->key_count = split_index;
    for (i = 0; i < leaf->key_count; i++) {
        leaf->keys[i] = temp_keys[i];
        leaf->data.leaf.offsets[i] = temp_offsets[i];
    }

    right->key_count = total_count - split_index;
    for (i = 0; i < right->key_count; i++) {
        right->keys[i] = temp_keys[split_index + i];
        right->data.leaf.offsets[i] = temp_offsets[split_index + i];
    }

    right->data.leaf.next = leaf->data.leaf.next;
    leaf->data.leaf.next = right;

    result.split = 1;
    result.promoted_key = right->keys[0];
    result.right_node = right;
    return result;
}

/* leaf 노드 삽입을 처리하고 필요 시 분할까지 수행한다.
 *
 * 입력:
 * - leaf: 삽입 대상 leaf 노드
 * - key: 저장할 키 값
 * - offset: key에 대응하는 CSV row offset
 * 출력:
 * - 반환값: 중복 여부, 실패 여부, 분할 결과를 담은 InsertResult
 */
static InsertResult insert_into_leaf(BPlusTreeNode *leaf, int key, long offset)
{
    InsertResult result;
    int position;

    memset(&result, 0, sizeof(result));
    position = find_leaf_position(leaf, key);
    if (position < leaf->key_count && leaf->keys[position] == key) {
        result.duplicate = 1;
        return result;
    }

    if (leaf->key_count < BPTREE_MAX_KEYS) {
        insert_leaf_without_split(leaf, position, key, offset);
        return result;
    }

    return split_leaf_and_insert(leaf, key, offset);
}

/* 여유가 있는 internal 노드에 promoted key와 오른쪽 자식을 끼워 넣는다.
 *
 * 입력:
 * - node: 삽입 대상 internal 노드
 * - position: 키를 넣을 위치
 * - promoted_key: 아래 자식 분할로 올라온 키
 * - right_child: 새로 생긴 오른쪽 자식 노드
 * 출력:
 * - 반환값 없음
 * - node: keys/children 배열이 밀리며 새 경계 키가 반영됨
 */
static void insert_internal_without_split(BPlusTreeNode *node,
                                          int position,
                                          int promoted_key,
                                          BPlusTreeNode *right_child)
{
    int i;

    for (i = node->key_count; i > position; i--) {
        node->keys[i] = node->keys[i - 1];
    }

    for (i = node->key_count + 1; i > position + 1; i--) {
        node->data.internal.children[i] = node->data.internal.children[i - 1];
    }

    node->keys[position] = promoted_key;
    node->data.internal.children[position + 1] = right_child;
    node->key_count += 1;
}

/* 가득 찬 internal 노드를 분할하면서 promoted key를 삽입한다.
 *
 * 입력:
 * - node: 분할할 원래 internal 노드
 * - position: 새 키를 넣을 위치
 * - promoted_key: 아래에서 올라온 키
 * - right_child: 새 오른쪽 자식 노드
 * 출력:
 * - 반환값: 부모로 다시 올릴 키와 오른쪽 internal 노드를 담은 결과 구조체
 * - node/right_node: 성공 시 두 internal 노드로 재배치됨
 */
static InsertResult split_internal_and_insert(BPlusTreeNode *node,
                                              int position,
                                              int promoted_key,
                                              BPlusTreeNode *right_child)
{
    InsertResult result;
    BPlusTreeNode *right;
    int temp_keys[BPTREE_MAX_KEYS + 1];
    BPlusTreeNode *temp_children[BPTREE_ORDER + 1];
    int total_keys;
    int split_index;
    int i;

    memset(&result, 0, sizeof(result));
    memset(temp_children, 0, sizeof(temp_children));

    right = create_node(0);
    if (right == NULL) {
        result.failed = 1;
        return result;
    }

    for (i = 0; i < node->key_count; i++) {
        temp_keys[i] = node->keys[i];
    }

    for (i = 0; i <= node->key_count; i++) {
        temp_children[i] = node->data.internal.children[i];
    }

    for (i = node->key_count; i > position; i--) {
        temp_keys[i] = temp_keys[i - 1];
    }
    temp_keys[position] = promoted_key;

    for (i = node->key_count + 1; i > position + 1; i--) {
        temp_children[i] = temp_children[i - 1];
    }
    temp_children[position + 1] = right_child;

    /*
     * internal split은 "안내판 역할"을 하는 경계 key를 다시 나누는 과정입니다.
     * 가운데 key 하나는 부모가 직접 들고, 그 왼쪽 자식들은 현재 노드에,
     * 오른쪽 자식들은 새 right 노드에 남겨 탐색 길을 두 갈래로 나눕니다.
     */
    total_keys = node->key_count + 1;
    split_index = total_keys / 2;

    node->key_count = split_index;
    for (i = 0; i < node->key_count; i++) {
        node->keys[i] = temp_keys[i];
    }
    for (i = 0; i <= node->key_count; i++) {
        node->data.internal.children[i] = temp_children[i];
    }

    right->key_count = total_keys - split_index - 1;
    for (i = 0; i < right->key_count; i++) {
        right->keys[i] = temp_keys[split_index + 1 + i];
    }
    for (i = 0; i <= right->key_count; i++) {
        right->data.internal.children[i] = temp_children[split_index + 1 + i];
    }

    result.split = 1;
    result.promoted_key = temp_keys[split_index];
    result.right_node = right;
    return result;
}

/* 현재 노드에서 시작해 재귀적으로 내려가며 삽입과 분할 전파를 처리한다.
 *
 * 입력:
 * - node: 현재 방문 중인 노드
 * - key: 삽입할 키 값
 * - offset: key에 대응하는 CSV row offset
 * 출력:
 * - 반환값: 하위 분할 결과와 오류 상태를 담은 InsertResult
 */
static InsertResult insert_recursive(BPlusTreeNode *node, int key, long offset)
{
    InsertResult child_result;
    int child_position;

    if (node->is_leaf) {
        return insert_into_leaf(node, key, offset);
    }

    child_position = find_child_position(node, key);
    child_result = insert_recursive(node->data.internal.children[child_position],
                                    key,
                                    offset);
    if (!child_result.split || child_result.duplicate || child_result.failed) {
        return child_result;
    }

    if (node->key_count < BPTREE_MAX_KEYS) {
        insert_internal_without_split(node,
                                      child_position,
                                      child_result.promoted_key,
                                      child_result.right_node);
        child_result.split = 0;
        child_result.right_node = NULL;
        return child_result;
    }

    return split_internal_and_insert(node,
                                     child_position,
                                     child_result.promoted_key,
                                     child_result.right_node);
}

/* 트리에 key -> offset 쌍을 삽입하고 필요 시 루트 분할을 처리한다.
 *
 * 입력:
 * - tree: 삽입 대상 B+ Tree
 * - key: PK 값
 * - offset: CSV 행 시작 오프셋
 * 출력:
 * - 반환값: 삽입 성공 시 1, 중복 키나 메모리 부족이면 0
 * - tree: 성공 시 루트가 새로 생기거나 내부 구조가 갱신될 수 있음
 */
int bptree_insert(BPlusTree *tree, int key, long offset)
{
    InsertResult result;
    BPlusTreeNode *new_root;

    if (tree == NULL) {
        return 0;
    }

    if (tree->root == NULL) {
        tree->root = create_node(1);
        if (tree->root == NULL) {
            return 0;
        }
    }

    result = insert_recursive(tree->root, key, offset);
    if (result.duplicate || result.failed) {
        return 0;
    }

    if (result.split) {
        new_root = create_node(0);
        if (new_root == NULL) {
            return 0;
        }

        new_root->keys[0] = result.promoted_key;
        new_root->data.internal.children[0] = tree->root;
        new_root->data.internal.children[1] = result.right_node;
        new_root->key_count = 1;
        tree->root = new_root;
    }

    return 1;
}

/* 단일 key를 검색해 대응하는 row offset을 찾는다.
 *
 * 입력:
 * - tree: 검색 대상 B+ Tree
 * - key: 찾을 키 값
 * - out_offset: 찾은 오프셋을 저장할 포인터, NULL 허용
 * 출력:
 * - 반환값: key를 찾으면 1, 없으면 0
 * - out_offset: 성공 시 대응 offset이 저장됨
 */
int bptree_search(const BPlusTree *tree, int key, long *out_offset)
{
    const BPlusTreeNode *node;
    int position;

    if (tree == NULL || tree->root == NULL) {
        return 0;
    }

    node = tree->root;
    while (!node->is_leaf) {
        position = find_child_position(node, key);
        node = node->data.internal.children[position];
    }

    position = find_leaf_position(node, key);
    if (position >= node->key_count || node->keys[position] != key) {
        return 0;
    }

    if (out_offset != NULL) {
        *out_offset = node->data.leaf.offsets[position];
    }

    return 1;
}

/* 트리에서 가장 왼쪽 leaf 노드를 찾아 순차 순회 시작점으로 사용한다.
 *
 * 입력:
 * - tree: 검색 대상 B+ Tree
 * 출력:
 * - 반환값: 가장 왼쪽 leaf 노드 포인터, 비어 있으면 NULL
 */
static const BPlusTreeNode *leftmost_leaf(const BPlusTree *tree)
{
    const BPlusTreeNode *node;

    if (tree == NULL || tree->root == NULL) {
        return NULL;
    }

    node = tree->root;
    while (!node->is_leaf) {
        node = node->data.internal.children[0];
    }

    return node;
}

/* 특정 key가 속할 leaf 노드를 찾는다.
 *
 * 입력:
 * - tree: 검색 대상 B+ Tree
 * - key: 기준 키 값
 * 출력:
 * - 반환값: key가 위치할 leaf 노드 포인터, 비어 있으면 NULL
 */
static const BPlusTreeNode *find_leaf(const BPlusTree *tree, int key)
{
    const BPlusTreeNode *node;
    int position;

    if (tree == NULL || tree->root == NULL) {
        return NULL;
    }

    node = tree->root;
    while (!node->is_leaf) {
        position = find_child_position(node, key);
        node = node->data.internal.children[position];
    }

    return node;
}

/* key보다 큰 값들을 leaf 연결 리스트를 따라 순서대로 방문한다.
 *
 * 입력:
 * - tree: 검색 대상 B+ Tree
 * - key: 하한 기준 키
 * - visit: 각 key-offset 쌍마다 호출할 콜백
 * - user_data: 콜백으로 전달할 사용자 데이터
 * 출력:
 * - 반환값: 전체 순회 성공 시 1, 콜백 중단 또는 오류 시 0
 */
int bptree_visit_greater_than(const BPlusTree *tree,
                              int key,
                              BptreeVisitFn visit,
                              void *user_data)
{
    const BPlusTreeNode *node;
    int position;
    int i;

    if (visit == NULL) {
        return 0;
    }

    node = find_leaf(tree, key);
    if (node == NULL) {
        return 1;
    }

    position = find_leaf_position(node, key);
    while (position < node->key_count && node->keys[position] <= key) {
        position += 1;
    }

    while (node != NULL) {
        for (i = position; i < node->key_count; i++) {
            if (!visit(node->keys[i], node->data.leaf.offsets[i], user_data)) {
                return 0;
            }
        }

        node = node->data.leaf.next;
        position = 0;
    }

    return 1;
}

/* key보다 작은 값들을 가장 왼쪽 leaf부터 차례로 방문한다.
 *
 * 입력:
 * - tree: 검색 대상 B+ Tree
 * - key: 상한 기준 키
 * - visit: 각 key-offset 쌍마다 호출할 콜백
 * - user_data: 콜백으로 전달할 사용자 데이터
 * 출력:
 * - 반환값: 전체 순회 성공 시 1, 콜백 중단 또는 오류 시 0
 */
int bptree_visit_less_than(const BPlusTree *tree,
                           int key,
                           BptreeVisitFn visit,
                           void *user_data)
{
    const BPlusTreeNode *node;
    int i;

    if (visit == NULL) {
        return 0;
    }

    node = leftmost_leaf(tree);
    while (node != NULL) {
        for (i = 0; i < node->key_count; i++) {
            if (node->keys[i] >= key) {
                return 1;
            }

            if (!visit(node->keys[i], node->data.leaf.offsets[i], user_data)) {
                return 0;
            }
        }

        node = node->data.leaf.next;
    }

    return 1;
}

#ifndef BPTREE_H
#define BPTREE_H

/*
 * 메모리 기반 B+ Tree 인덱스입니다.
 *
 * key는 테이블의 PK(id), value는 CSV 파일에서 해당 row가 시작되는
 * file offset입니다. 즉 인덱스는 row 전체가 아니라 "찾아갈 위치"만
 * 저장합니다.
 */

#define BPTREE_ORDER 4

typedef struct BPlusTree BPlusTree;
typedef int (*BptreeVisitFn)(int key, long offset, void *user_data);

BPlusTree *bptree_create(void);
void bptree_destroy(BPlusTree *tree);
int bptree_insert(BPlusTree *tree, int key, long offset);
int bptree_search(const BPlusTree *tree, int key, long *out_offset);
int bptree_visit_greater_than(const BPlusTree *tree,
                              int key,
                              BptreeVisitFn visit,
                              void *user_data);
int bptree_visit_less_than(const BPlusTree *tree,
                           int key,
                           BptreeVisitFn visit,
                           void *user_data);

#endif

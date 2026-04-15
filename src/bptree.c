#include <stdlib.h>
#include <string.h>

#include "bptree.h"

#define BPTREE_MAX_KEYS (BPTREE_ORDER - 1)

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

BPlusTree *bptree_create(void)
{
    BPlusTree *tree;

    tree = (BPlusTree *)calloc(1, sizeof(*tree));
    return tree;
}

void bptree_destroy(BPlusTree *tree)
{
    if (tree == NULL) {
        return;
    }

    destroy_node(tree->root);
    free(tree);
}

static int find_leaf_position(const BPlusTreeNode *node, int key)
{
    int position;

    position = 0;
    while (position < node->key_count && node->keys[position] < key) {
        position += 1;
    }

    return position;
}

static int find_child_position(const BPlusTreeNode *node, int key)
{
    int position;

    position = 0;
    while (position < node->key_count && key >= node->keys[position]) {
        position += 1;
    }

    return position;
}

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

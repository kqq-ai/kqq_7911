#ifndef __TREE_MAP_H__
#define __TREE_MAP_H__

#include "cpu.h"
#include "rwlock.h"

// 定义颜色枚举
typedef enum { RED, BLACK } Color;

// 定义红黑树节点结构
typedef struct RBTreeNode {
    char *key;
    void *value;
    Color color;
    struct RBTreeNode *left;
    struct RBTreeNode *right;
    struct RBTreeNode *parent;
} RBTreeNode;

// 定义迭代器结构
typedef struct {
    struct Map *map;
    RBTreeNode *current;
} MapIterator;

// 定义map结构
typedef struct Map {
    RBTreeNode *root;
    RBTreeNode *nil;  // 哨兵节点
    size_t size;
    rwlock_t rwlock;  // 读写锁
} Map;

// 创建和销毁map
Map* map_create();
void map_destroy(Map *map);

// 基本操作
bool map_insert(Map *map, const char *key, void *value);
void* map_find(Map *map, const char *key);
bool map_erase(Map *map, const char *key);
size_t map_size(Map *map);
void map_clear(Map *map, void (*value_free_cb)(void *));

// 迭代器相关
MapIterator map_begin(Map *map);
MapIterator map_end(Map *map);
void map_iterator_next(MapIterator *it);
bool map_iterator_equal(MapIterator it1, MapIterator it2);
char* map_iterator_key(MapIterator it);
void* map_iterator_value(MapIterator it);

#endif // __TREE_MAP_H__
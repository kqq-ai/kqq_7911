#include "tree_map.h"


void rwlock_acquire_read(rwlock_t *rwlock);
void rwlock_release_read(rwlock_t *rwlock);
void rwlock_acquire_write(rwlock_t *rwlock);
void rwlock_release_write(rwlock_t *rwlock);

// 创建哨兵节点
static RBTreeNode* create_nil_node() {
    RBTreeNode *node = (RBTreeNode*)malloc(sizeof(RBTreeNode));
    node->color = BLACK;
    node->left = node->right = node->parent = NULL;
    node->key = NULL;
    node->value = NULL;
    return node;
}

// 创建新节点
static RBTreeNode* create_node(const char *key, void *value, Color color, RBTreeNode *nil) {
    RBTreeNode *node = (RBTreeNode*)malloc(sizeof(RBTreeNode));
    node->key = strdup(key);
    node->value = value;
    node->color = color;
    node->left = node->right = node->parent = nil;
    return node;
}

// 创建map
Map* map_create() {
    Map *map = (Map*)malloc(sizeof(Map));
    map->nil = create_nil_node();
    map->root = map->nil;
    map->size = 0;
    rwlock_init(&map->rwlock);
    return map;
}

// 销毁节点及其子树
static void destroy_node(RBTreeNode *node, RBTreeNode *nil) {
    if (node != nil) {
        destroy_node(node->left, nil);
        destroy_node(node->right, nil);
        free(node->key);
        free(node);
    }
}

// 销毁map
void map_destroy(Map *map) {
    rwlock_acquire_write(&map->rwlock);
    destroy_node(map->root, map->nil);
    free(map->nil);
    rwlock_release_write(&map->rwlock);
    rwlock_destroy(&map->rwlock);
    free(map);
}

// 左旋 (内部使用，不单独加锁)
static void left_rotate(Map *map, RBTreeNode *x) {
    RBTreeNode *y = x->right;
    x->right = y->left;
    
    if (y->left != map->nil) {
        y->left->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == map->nil) {
        map->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    
    y->left = x;
    x->parent = y;
}

// 右旋 (内部使用，不单独加锁)
static void right_rotate(Map *map, RBTreeNode *y) {
    RBTreeNode *x = y->left;
    y->left = x->right;
    
    if (x->right != map->nil) {
        x->right->parent = y;
    }
    
    x->parent = y->parent;
    
    if (y->parent == map->nil) {
        map->root = x;
    } else if (y == y->parent->right) {
        y->parent->right = x;
    } else {
        y->parent->left = x;
    }
    
    x->right = y;
    y->parent = x;
}

// 插入修复 (内部使用，不单独加锁)
static void insert_fixup(Map *map, RBTreeNode *z) {
    while (z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            RBTreeNode *y = z->parent->parent->right;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    left_rotate(map, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                right_rotate(map, z->parent->parent);
            }
        } else {
            RBTreeNode *y = z->parent->parent->left;
            if (y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(map, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                left_rotate(map, z->parent->parent);
            }
        }
    }
    map->root->color = BLACK;
}

// 插入元素
bool map_insert(Map *map, const char *key, void *value) {
    rwlock_acquire_write(&map->rwlock);
    
    RBTreeNode *y = map->nil;
    RBTreeNode *x = map->root;
    
    while (x != map->nil) {
        y = x;
        int cmp = strcmp(key, x->key);
        if (cmp == 0) {
            // 键已存在，更新值
            x->value = value;
            rwlock_release_write(&map->rwlock);
            return true;
        } else if (cmp < 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    
    RBTreeNode *z = create_node(key, value, RED, map->nil);
    z->parent = y;
    
    if (y == map->nil) {
        map->root = z;
    } else if (strcmp(key, y->key) < 0) {
        y->left = z;
    } else {
        y->right = z;
    }
    
    insert_fixup(map, z);
    map->size++;
    
    rwlock_release_write(&map->rwlock);
    return true;
}

// 查找节点 (内部使用，不单独加锁)
static RBTreeNode* find_node(Map *map, const char *key) {
    RBTreeNode *x = map->root;
    while (x != map->nil) {
        int cmp = strcmp(key, x->key);
        if (cmp == 0) {
            return x;
        } else if (cmp < 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    return NULL;
}

// 查找元素
void* map_find(Map *map, const char *key) {
    rwlock_acquire_read(&map->rwlock);
    RBTreeNode *node = find_node(map, key);
    void *value = node ? node->value : NULL;
    rwlock_release_read(&map->rwlock);
    return value;
}

// 查找最小节点 (内部使用，不单独加锁)
static RBTreeNode* minimum(Map *map, RBTreeNode *x) {
    while (x->left != map->nil) {
        x = x->left;
    }
    return x;
}

// 移植子树 (内部使用，不单独加锁)
static void transplant(Map *map, RBTreeNode *u, RBTreeNode *v) {
    if (u->parent == map->nil) {
        map->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    v->parent = u->parent;
}

// 删除修复 (内部使用，不单独加锁)
static void delete_fixup(Map *map, RBTreeNode *x) {
    while (x != map->root && x->color == BLACK) {
        if (x == x->parent->left) {
            RBTreeNode *w = x->parent->right;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                left_rotate(map, x->parent);
                w = x->parent->right;
            }
            if (w->left->color == BLACK && w->right->color == BLACK) {
                w->color = RED;
                x = x->parent;
            } else {
                if (w->right->color == BLACK) {
                    w->left->color = BLACK;
                    w->color = RED;
                    right_rotate(map, w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->right->color = BLACK;
                left_rotate(map, x->parent);
                x = map->root;
            }
        } else {
            RBTreeNode *w = x->parent->left;
            if (w->color == RED) {
                w->color = BLACK;
                x->parent->color = RED;
                right_rotate(map, x->parent);
                w = x->parent->left;
            }
            if (w->right->color == BLACK && w->left->color == BLACK) {
                w->color = RED;
                x = x->parent;
            } else {
                if (w->left->color == BLACK) {
                    w->right->color = BLACK;
                    w->color = RED;
                    left_rotate(map, w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                w->left->color = BLACK;
                right_rotate(map, x->parent);
                x = map->root;
            }
        }
    }
    x->color = BLACK;
}

// 删除元素
bool map_erase(Map *map, const char *key) {
    rwlock_acquire_write(&map->rwlock);
    
    RBTreeNode *z = find_node(map, key);
    if (!z) {
        rwlock_release_write(&map->rwlock);
        return false;
    }

    RBTreeNode *y = z;
    RBTreeNode *x;
    Color y_original_color = y->color;
    
    if (z->left == map->nil) {
        x = z->right;
        transplant(map, z, z->right);
    } else if (z->right == map->nil) {
        x = z->left;
        transplant(map, z, z->left);
    } else {
        y = minimum(map, z->right);
        y_original_color = y->color;
        x = y->right;
        
        if (y->parent == z) {
            x->parent = y;
        } else {
            transplant(map, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        
        transplant(map, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    
    if (y_original_color == BLACK) {
        delete_fixup(map, x);
    }
    
    free(z->key);
    free(z);
    map->size--;
    
    rwlock_release_write(&map->rwlock);
    return true;
}

// 获取map大小
size_t map_size(Map *map) {
    rwlock_acquire_read(&map->rwlock);
    size_t size = map->size;
    rwlock_release_read(&map->rwlock);
    return size;
}

// 获取起始迭代器
MapIterator map_begin(Map *map) {
    rwlock_acquire_read(&map->rwlock);
    
    MapIterator it;
    it.map = map;
    it.current = map->root;
    
    while (it.current != map->nil && it.current->left != map->nil) {
        it.current = it.current->left;
    }
    
    // 注意：迭代器使用期间map仍被读锁定
    return it;
}

// 获取结束迭代器
MapIterator map_end(Map *map) {
    MapIterator it;
    it.map = map;
    it.current = map->nil;
    return it;
}

// 迭代器前进
void map_iterator_next(MapIterator *it) {
    if (it->current == it->map->nil) return;
    
    if (it->current->right != it->map->nil) {
        it->current = it->current->right;
        while (it->current->left != it->map->nil) {
            it->current = it->current->left;
        }
    } else {
        RBTreeNode *y = it->current->parent;
        while (y != it->map->nil && it->current == y->right) {
            it->current = y;
            y = y->parent;
        }
        it->current = y;
    }
}

// 比较迭代器
bool map_iterator_equal(MapIterator it1, MapIterator it2) {
    return it1.map == it2.map && it1.current == it2.current;
}

// 获取迭代器键
char* map_iterator_key(MapIterator it) {
    return it.current != it.map->nil ? it.current->key : NULL;
}

// 获取迭代器值
void* map_iterator_value(MapIterator it) {
    return it.current != it.map->nil ? it.current->value : NULL;
}

// 释放迭代器锁
void map_iterator_unlock(MapIterator it) {
    rwlock_release_read(&it.map->rwlock);
}
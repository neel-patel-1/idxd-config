#pragma once
#include "accel_test.h"
#include "dsa.h"
#include "iaa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <linux/mman.h>


#define MAX_KEYS 3

typedef struct Node {
    int keys[MAX_KEYS];
    struct Node *children[MAX_KEYS + 1];
    int num_keys;
    struct Node *parent;
} Node;

Node *create_node() {
    Node *node = malloc(sizeof(Node));
    node->num_keys = 0;
    node->parent = NULL;
    for (int i = 0; i < MAX_KEYS + 1; i++) {
        node->children[i] = NULL;
    }
    return node;
}

void insert_key(Node *node, int key) {
    int i;
    for (i = 0; i < node->num_keys; i++) {
        if (key < node->keys[i]) {
            break;
        }
    }
    for (int j = node->num_keys; j > i; j--) {
        node->keys[j] = node->keys[j - 1];
        node->children[j + 1] = node->children[j];
    }
    node->keys[i] = key;
    node->children[i + 1] = node->children[i];
    node->num_keys++;
}


void split_node(Node *node) {
    Node *new_node = create_node();
    new_node->parent = node->parent;
    if (node->parent) {
        insert_key(node->parent, node->keys[MAX_KEYS / 2]);
    }
    new_node->children[0] = node->children[MAX_KEYS / 2 + 1];
    node->children[MAX_KEYS / 2 + 1] = NULL;
    node->num_keys = MAX_KEYS / 2;
    for (int i = MAX_KEYS / 2 + 1; i < MAX_KEYS; i++) {
        insert_key(new_node, node->keys[i]);
        node->children[i] = NULL;
    }
    node->num_keys = MAX_KEYS / 2;
}

Node *find_leaf(Node *root, int key) {
    Node *node = root;
    while (node->children[0] != NULL) {
        int i;
        for (i = 0; i < node->num_keys; i++) {
            if (key < node->keys[i]) {
                break;
            }
        }
        node = node->children[i];
    }
    return node;
}

void insert(Node *root, int key) {
    Node *leaf = find_leaf(root, key);
    if (leaf->num_keys < MAX_KEYS) {
        insert_key(leaf, key);
    } else {
        split_node(leaf);
        insert(root, key);
    }
}
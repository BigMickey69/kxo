#pragma once

#include <linux/list.h>

#include "game.h"

#define HASH_TABLE_SIZE (100003)

extern u64 zobrist_table[N_GRIDS][2];

typedef struct {
    u64 key;
    int score;
    int move;
    struct hlist_node ht_list;
} zobrist_entry_t;

void zobrist_init(void);
zobrist_entry_t *zobrist_get(u64 key);
void zobrist_put(u64 key, int score, int move);
void zobrist_clear(void);
void zobrist_free(void);
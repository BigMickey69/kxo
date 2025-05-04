#pragma once

#define BOARD_SIZE 4
#define GOAL 3
#define ALLOW_EXCEED 1
#define N_GRIDS (BOARD_SIZE * BOARD_SIZE)
#define GET_INDEX(i, j) ((i) * (BOARD_SIZE) + (j))
#define GET_COL(x) ((x) % BOARD_SIZE)
#define GET_ROW(x) ((x) / BOARD_SIZE)

#define for_each_empty_grid(i, table) \
    for (int i = 0; i < N_GRIDS; i++) \
        if (table[i] == ' ')

typedef struct {
    int i_shift, j_shift;
    int i_lower_bound, j_lower_bound, i_upper_bound, j_upper_bound;
} line_t;

/* Self-defined fixed-point type, using last 10 bits as fractional bits,
 * starting from lsb */
#define FIXED_SCALE_BITS 8
#define FIXED_MAX (~0U)
#define FIXED_MIN (0U)
#define GET_SIGN(x) ((x) & (1U << 31))
#define SET_SIGN(x) ((x) | (1U << 31))
#define CLR_SIGN(x) ((x) & ((1U << 31) - 1U))
typedef unsigned fixed_point_t;

#define DRAW_SIZE (N_GRIDS + BOARD_SIZE)
#define DRAWBUFFER_SIZE                                                 \
    ((BOARD_SIZE * (BOARD_SIZE + 1) << 1) + (BOARD_SIZE * BOARD_SIZE) + \
     (BOARD_SIZE << 1) + 1)

#define READ_DATA_SIZE 2

extern const line_t lines[4];

int *available_moves(const char *table);
char check_win(const char *t);
fixed_point_t calculate_win_value(char win, char player);


struct game {
    unsigned char id;
    char turn;
    unsigned char finish;
    unsigned char last_move;
    char table[N_GRIDS];
};
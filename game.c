#include "game.h"
#include <linux/slab.h>


const line_t lines[4] = {
    {1, 0, 0, 0, BOARD_SIZE - GOAL + 1, BOARD_SIZE},             // ROW
    {0, 1, 0, 0, BOARD_SIZE, BOARD_SIZE - GOAL + 1},             // COL
    {1, 1, 0, 0, BOARD_SIZE - GOAL + 1, BOARD_SIZE - GOAL + 1},  // PRIMARY
    {1, -1, 0, GOAL - 1, BOARD_SIZE - GOAL + 1, BOARD_SIZE},     // SECONDARY
};

static char check_line_segment_win(const char *t, int i, int j, line_t line)
{
    char last = t[GET_INDEX(i, j)];
    if (last == ' ')
        return ' ';
    for (int k = 1; k < GOAL; k++) {
        if (last != t[GET_INDEX(i + k * line.i_shift, j + k * line.j_shift)])
            return ' ';
    }

#if !ALLOW_EXCEED
    if (last == LOOKUP(t, i - line.i_shift, j - line.j_shift, ' ') ||
        last ==
            LOOKUP(t, i + GOAL * line.i_shift, j + GOAL * line.j_shift, ' '))
        return ' ';
#endif
    return last;
}

char check_win(const char *t)
{
    for (int i_line = 0; i_line < 4; ++i_line) {
        line_t line = lines[i_line];
        for (int i = line.i_lower_bound; i < line.i_upper_bound; ++i) {
            for (int j = line.j_lower_bound; j < line.j_upper_bound; ++j) {
                char win = check_line_segment_win(t, i, j, line);
                if (win != ' ')
                    return win;
            }
        }
    }
    for (int i = 0; i < N_GRIDS; i++)
        if (t[i] == ' ')
            return ' ';
    return 'D';
}

fixed_point_t calculate_win_value(char win, char player)
{
    if (win == player)
        return 1U << FIXED_SCALE_BITS;
    if (win == (player ^ 'O' ^ 'X'))
        return 0U;
    return 1U << (FIXED_SCALE_BITS - 1);
}

int *available_moves(const char *table)
{
    int *moves = kzalloc(N_GRIDS * sizeof(int), GFP_KERNEL);
    int m = 0;
    for (int i = 0; i < N_GRIDS; i++)
        if (table[i] == ' ')
            moves[m++] = i;
    if (m < N_GRIDS)
        moves[m] = -1;
    return moves;
}

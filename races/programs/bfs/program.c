/* Pathfinding as a game does it: breadth-first search on a 64 x 48 grid of floor and
 * wall (walls all round the edge, so no bounds checks), a queue of cell numbers, each
 * cell's direction back toward the start in came_from; then the path is walked back from
 * the goal and marked in the grid. Six searches, each over the grid the last one marked;
 * lengths gets each path's length (65535 for none), checksum folds them and the grid. */
#include <string.h>
#include "race.h"

#define W 64
#define H 48
#define CELLS (W * H)
#define SEARCHES 6
#define WALL 1
#define PATH 2
#define UNSEEN 255
#define START 4

static const int16_t STEP[4] = {-W, 1, W, -1}; /* north, east, south, west */

static uint16_t tail;

static void visit(uint16_t next, uint8_t d) {
    if (grid[next] != WALL && came_from[next] == UNSEEN) {
        came_from[next] = d;
        queue[tail++] = next;
    }
}

static uint16_t find_path(uint16_t start, uint16_t goal) {
    uint16_t head, cell, len;
    memset(came_from, UNSEEN, CELLS);
    came_from[start] = START;
    head = 0;
    tail = 0;
    queue[tail++] = start;
    while (head != tail) {
        cell = queue[head++];
        if (cell == goal) break;
        visit(cell - W, 0);
        visit(cell + 1, 1);
        visit(cell + W, 2);
        visit(cell - 1, 3);
    }
    if (came_from[goal] == UNSEEN) return 0xFFFF;
    len = 0;
    cell = goal;
    while (cell != start) {
        grid[cell] = PATH;
        cell -= STEP[came_from[cell]];
        len++;
    }
    return len;
}

int main(void) {
    uint16_t i, sum_a = 0, sum_b = 0;
    uint8_t s;
    for (s = 0; s < SEARCHES; s++) {
        lengths[s] = find_path(starts[s], goals[s]);
        sum_a += lengths[s];
        sum_b += sum_a;
    }
    for (i = 0; i < CELLS; i++) {
        sum_a += grid[i];
        sum_b += sum_a;
    }
    checksum = sum_a ^ sum_b;
    return 0;
}

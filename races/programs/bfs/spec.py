"""Pathfinding: breadth-first search on a 64 x 48 grid, 6 searches, paths marked"""

from kernels import Kernel, RESULT
from layout import Buf

W, H = 64, 48
CELLS = W * H
SEARCHES = 6
FLOOR, WALL, PATH = 0, 1, 2
UNSEEN, START = 255, 4
STEP = [-W, 1, W, -1]  # north, east, south, west


def make_grid(rng):
    grid = []
    for y in range(H):
        for x in range(W):
            edge = x in (0, W - 1) or y in (0, H - 1)
            grid.append(WALL if edge or rng.below(100) < 28 else FLOOR)
    return grid


def inputs(rng):
    grid = make_grid(rng)
    floors = [c for c in range(CELLS) if grid[c] == FLOOR]
    ends = [floors[rng.below(len(floors))] for _ in range(2 * SEARCHES)]
    return {"grid": grid, "came_from": [0x5A] * CELLS, "queue": [0x5A5A] * CELLS,
            "starts": ends[:SEARCHES], "goals": ends[SEARCHES:],
            "lengths": [0x5A5A] * SEARCHES, "checksum": 0x5A5A}


def reference(ins):
    grid = list(ins["grid"])
    lengths = []
    came = None
    queue = list(ins["queue"])
    for start, goal in zip(ins["starts"], ins["goals"]):
        came = [UNSEEN] * CELLS
        came[start] = START
        head = tail = 0
        queue[tail] = start
        tail += 1
        while head != tail:
            cell = queue[head]
            head += 1
            if cell == goal:
                break
            for d in range(4):
                nxt = cell + STEP[d]
                if grid[nxt] != WALL and came[nxt] == UNSEEN:
                    came[nxt] = d
                    queue[tail] = nxt
                    tail += 1
        if came[goal] == UNSEEN:
            lengths.append(0xFFFF)
            continue
        n = 0
        cell = goal
        while cell != start:
            grid[cell] = PATH
            cell -= STEP[came[cell]]
            n += 1
        lengths.append(n)
    a = b = 0
    for v in lengths + grid:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return {"grid": grid, "came_from": came, "lengths": lengths, "checksum": a ^ b}


PROGRAM = Kernel("bfs", "Pathfinding: breadth-first search on a 64 x 48 grid, 6 paths",
                 [Buf("grid", "u8", CELLS, 0x2000), Buf("came_from", "u8", CELLS, 0x3000),
                  Buf("queue", "u16", CELLS, 0x4000), Buf("starts", "u16", SEARCHES, 0x5E00),
                  Buf("goals", "u16", SEARCHES, 0x5E20), Buf("lengths", "u16", SEARCHES, 0x5E40),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)

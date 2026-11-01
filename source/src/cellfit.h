#pragma once
// Which exterior cell a position belongs to, as the engine computes it (2026-10-31, the 16:07 hang).
// FUN_140595DF0 (cell at a position) multiplies x and y by 0.01f (read from the exe: cells are 100 m) and floors.
// A reference must stay listed in the cell its position is in. The 16:07 hang: a job processing a cell holds that
// cell's reference read lock (cell+0x118) while helper jobs load each non-actor reference's 3D; a helper whose
// reference is positioned outside the cell calls the mover (FUN_140B52E30 -> FUN_140B48CA0), which needs the same
// cell's write lock, and waits forever. FVO's loaded-area sync had written vehicle positions past their cell with the
// position-only setter FUN_140B4E410, which never changes the parent cell.
#include <cmath>

namespace cellfit {
inline int Grid(float metres) { return static_cast<int>(std::floor(metres * 0.01f)); }
// True when (x, y) lies in exterior cell (cx, cy).
inline bool Inside(int cx, int cy, float x, float y) { return Grid(x) == cx && Grid(y) == cy; }
}

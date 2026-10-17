#pragma once

#include <cstdint>

// The user's landing-cell layout: a rhombille lattice where every rhombus belongs to two "cube" cells
// (3 rhombi around a 3-way point) and two "star" cells (6 rhombi around a 6-way point). Same maths as the
// prototype simulator (prototype/rhombille-sim.html). Coordinates are metres in a planet-fixed frame.
namespace hexgrid
{
	struct CellId
	{
		std::int32_t i = 0, j = 0;  // lattice point at the cell's centre
		bool operator==(const CellId& o) const { return i == o.i && j == o.j; }
		bool operator!=(const CellId& o) const { return !(*this == o); }
		bool IsStar() const;        // centred on a 6-way point
	};

	struct Lattice
	{
		double side = 3600.0;  // rhombus side, metres

		void CentreOf(CellId c, double& x, double& y) const;

		// Distance from (x, y) to the cell's boundary; positive inside.
		double Depth(CellId c, double x, double y) const;

		// The four cells (2 cube + 2 star) whose rhombus contains (x, y).
		void Candidates(double x, double y, CellId out[4]) const;

		// The candidate the point is deepest inside.
		CellId Deepest(double x, double y, double* depth = nullptr) const;
	};
}

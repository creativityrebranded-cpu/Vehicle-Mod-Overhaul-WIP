#include "hexgrid.h"

#include <cmath>
#include <limits>

namespace hexgrid
{
	namespace
	{
		constexpr double kSqrt3 = 1.7320508075688772;

		int Coset(std::int32_t i, std::int32_t j)
		{
			return (((i - j) % 3) + 3) % 3;  // 0 = 6-way point (star centre), 1/2 = 3-way point (cube centre)
		}

		// Unit normals at 30 + 60k degrees.
		const double kDirs[6][2] = {
			{ 0.8660254037844387, 0.5 }, { 0.0, 1.0 }, { -0.8660254037844387, 0.5 },
			{ -0.8660254037844387, -0.5 }, { 0.0, -1.0 }, { 0.8660254037844387, -0.5 },
		};
	}

	bool CellId::IsStar() const { return Coset(i, j) == 0; }

	void Lattice::CentreOf(CellId c, double& x, double& y) const
	{
		x = side * (c.i + c.j / 2.0);
		y = side * c.j * kSqrt3 / 2.0;
	}

	double Lattice::Depth(CellId c, double x, double y) const
	{
		double cx, cy;
		CentreOf(c, cx, cy);
		const double dx = x - cx, dy = y - cy, r = side * kSqrt3 / 2.0;
		if (!c.IsStar()) {
			double m = -std::numeric_limits<double>::infinity();
			for (const auto& d : kDirs) {
				m = std::fmax(m, dx * d[0] + dy * d[1]);
			}
			return r - m;  // hexagon through the six neighbours
		}
		// Star = union of two triangles (tips at 30/150/270 and 90/210/330 degrees); use the deeper one.
		double m1 = -std::numeric_limits<double>::infinity(), m2 = m1;
		for (int k = 0; k < 6; ++k) {
			const double d = -(dx * kDirs[k][0] + dy * kDirs[k][1]);
			(k % 2 == 0 ? m1 : m2) = std::fmax(k % 2 == 0 ? m1 : m2, d);
		}
		return std::fmax(r - m1, r - m2);
	}

	void Lattice::Candidates(double x, double y, CellId out[4]) const
	{
		const double fj = y / (side * kSqrt3 / 2.0);
		const double fi = x / side - fj / 2.0;
		const auto i0 = static_cast<std::int32_t>(std::floor(fi));
		const auto j0 = static_cast<std::int32_t>(std::floor(fj));
		std::int32_t t[3][2];
		if ((fi - i0) + (fj - j0) < 1.0) {
			t[0][0] = i0; t[0][1] = j0; t[1][0] = i0 + 1; t[1][1] = j0; t[2][0] = i0; t[2][1] = j0 + 1;
		}
		else {
			t[0][0] = i0 + 1; t[0][1] = j0 + 1; t[1][0] = i0 + 1; t[1][1] = j0; t[2][0] = i0; t[2][1] = j0 + 1;
		}
		CellId a{}, b{}, z{};
		for (auto& p : t) {
			const int c = Coset(p[0], p[1]);
			(c == 1 ? a : c == 2 ? b : z) = CellId{ p[0], p[1] };
		}
		const CellId zOther{ a.i + b.i - z.i, a.j + b.j - z.j };
		out[0] = a;
		out[1] = b;
		out[2] = z;
		out[3] = zOther;
	}

	CellId Lattice::Deepest(double x, double y, double* depth) const
	{
		CellId c[4];
		Candidates(x, y, c);
		CellId best = c[0];
		double bd = -std::numeric_limits<double>::infinity();
		for (const auto& id : c) {
			const double d = Depth(id, x, y);
			if (d > bd) {
				bd = d;
				best = id;
			}
		}
		if (depth) {
			*depth = bd;
		}
		return best;
	}
}

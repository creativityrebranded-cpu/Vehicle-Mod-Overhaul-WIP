#include "sphere.h"

#include <cmath>

namespace sphere
{
	namespace
	{
		struct V3
		{
			double x, y, z;
			V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
			V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
			V3 operator*(double s) const { return { x * s, y * s, z * s }; }
			double Dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
			V3 Unit() const { const double l = std::sqrt(Dot(*this)); return { x / l, y / l, z / l }; }
		};

		constexpr double kPhi = 1.6180339887498949;

		// Regular icosahedron (vertices on the unit sphere) and its 20 faces, consistently wound.
		const V3 kRaw[12] = {
			{ -1, kPhi, 0 }, { 1, kPhi, 0 }, { -1, -kPhi, 0 }, { 1, -kPhi, 0 },
			{ 0, -1, kPhi }, { 0, 1, kPhi }, { 0, -1, -kPhi }, { 0, 1, -kPhi },
			{ kPhi, 0, -1 }, { kPhi, 0, 1 }, { -kPhi, 0, -1 }, { -kPhi, 0, 1 },
		};
		const int kFaces[20][3] = {
			{ 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
			{ 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
			{ 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
			{ 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
		};

		V3 Vertex(int i) { return kRaw[i].Unit(); }

		V3 Direction(double lat, double lon)
		{
			return { std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat) };
		}
	}

	double Angle(double lat1, double lon1, double lat2, double lon2)
	{
		const double d = Direction(lat1, lon1).Dot(Direction(lat2, lon2));
		return std::acos(d > 1.0 ? 1.0 : d < -1.0 ? -1.0 : d);
	}

	double EdgeAngle()
	{
		return std::acos(Vertex(0).Dot(Vertex(11)));
	}

	FacePoint Locate(double lat, double lon, int frequency)
	{
		const V3 v = Direction(lat, lon);
		// The face whose centre is closest to the direction.
		int best = 0;
		double bestDot = -2.0;
		for (int f = 0; f < 20; ++f) {
			const V3 c = (Vertex(kFaces[f][0]) + Vertex(kFaces[f][1]) + Vertex(kFaces[f][2])).Unit();
			const double d = c.Dot(v);
			if (d > bestDot) {
				bestDot = d;
				best = f;
			}
		}
		const V3 a = Vertex(kFaces[best][0]), b = Vertex(kFaces[best][1]), c = Vertex(kFaces[best][2]);
		// Gnomonic projection of the direction onto the face plane, then barycentric (u along a->b, w along a->c).
		const V3 n = (a + b + c).Unit();
		const V3 p = v * (a.Dot(n) / v.Dot(n));
		const V3 e1 = b - a, e2 = c - a, q = p - a;
		const double d11 = e1.Dot(e1), d12 = e1.Dot(e2), d22 = e2.Dot(e2);
		const double q1 = q.Dot(e1), q2 = q.Dot(e2);
		const double det = d11 * d22 - d12 * d12;
		const double u = (q1 * d22 - q2 * d12) / det;
		const double w = (q2 * d11 - q1 * d12) / det;
		// Lattice indices (i along e1, j along e2) -> the planar 60-degree frame the zone lattice uses.
		const double fi = u * frequency, fj = w * frequency;
		return { best, fi + fj / 2.0, fj * 0.8660254037844386 };
	}

	void ToLatLong(const FacePoint& fp, int frequency, double& lat, double& lon)
	{
		const int f = (fp.face >= 0 && fp.face < 20) ? fp.face : 0;
		const V3 a = Vertex(kFaces[f][0]), b = Vertex(kFaces[f][1]), c = Vertex(kFaces[f][2]);
		const double fj = fp.y / 0.8660254037844386;
		const double fi = fp.x - fj / 2.0;
		const double u = fi / frequency, w = fj / frequency;
		const V3 v = (a + (b - a) * u + (c - a) * w).Unit();  // back through the gnomonic projection
		lat = std::asin(v.z > 1.0 ? 1.0 : v.z < -1.0 ? -1.0 : v.z);
		lon = std::atan2(v.y, v.x);
	}
}

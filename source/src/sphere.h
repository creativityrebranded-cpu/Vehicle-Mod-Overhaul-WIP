#pragma once

#include <cstdint>

// Planet-scale geometry for zones. A planet is treated as a sphere tiled by a geodesic (subdivided icosahedron)
// triangle lattice: each of the 20 icosahedron faces carries the same 60-degree lattice the zone design is
// built on (six-way points = star zone centres, the other points = cube zone centres). Positions come from
// the engine's own world -> latitude/longitude conversion, so they are correct anywhere on the planet.
namespace sphere
{
	struct FacePoint
	{
		int face = -1;         // icosahedron face 0..19
		double x = 0, y = 0;   // planar lattice coordinates on that face (rhombus side = 1)
	};

	// Unit-sphere direction -> icosahedron face and lattice coordinates for a lattice of `frequency`
	// subdivisions per icosahedron edge.
	FacePoint Locate(double lat, double lon, int frequency);
	// The inverse: a lattice point on an icosahedron face -> latitude/longitude (radians).
	void ToLatLong(const FacePoint& fp, int frequency, double& lat, double& lon);

	// Great-circle angle between two latitude/longitude points (radians).
	double Angle(double lat1, double lon1, double lat2, double lon2);

	// Arc length of one icosahedron edge on the unit sphere (radians).
	double EdgeAngle();
}

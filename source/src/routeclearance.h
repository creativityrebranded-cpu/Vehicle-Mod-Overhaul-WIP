#pragma once
#include <cmath>

// Authoring policy: a mapped structure can be intentional (hangar, tunnel, covered road).
// Only a known terrain violation or invalid coordinate prevents editing; structures warn.
namespace routeclearance {
enum class Block { None, InvalidPosition, UnderTerrain, BelowUnknownFloor };
struct Advice { Block block = Block::None; bool structureWarning = false; };
inline Advice Judge(double x, double y, double z, bool terrainKnown, double ground, bool mappedSolid, double floor = 0) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || (terrainKnown && !std::isfinite(ground)))
        return {Block::InvalidPosition, false};
    if (terrainKnown && z < ground + 1) return {Block::UnderTerrain, mappedSolid};
    if (mappedSolid) return {Block::None, true};
    if (!terrainKnown && z < floor - 0.01) return {Block::BelowUnknownFloor, false};
    return {};
}
}

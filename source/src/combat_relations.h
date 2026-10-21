#pragma once
#include <cmath>
#include <cstdint>

// Game reaction values from GetFactionRelation: 0 neutral, 1 enemy, 2 ally, 3 friend.
// A live combat hostility overrides a faction friendship. Neutral actors never become squad mates merely by sharing an enemy.
namespace combatrelations {
enum class Relation : std::uint8_t { Neutral, Ally, Enemy };
inline Relation Classify(bool protectedPair, bool hostile, int ab, int ba)
{
    if (protectedPair) return Relation::Ally;
    if (hostile || ab == 1 || ba == 1) return Relation::Enemy;
    if (ab >= 2 && ab <= 3 && ba >= 2 && ba <= 3) return Relation::Ally;
    return Relation::Neutral;
}
template<int N> int Partition(int count, const Relation (&relations)[N][N], int (&group)[N])
{
    int groups = 0;
    for (int i = 0; i < count; ++i) {
        group[i] = -1;
        for (int g = 0; g < groups && group[i] < 0; ++g) {
            bool allAllies = true;
            for (int j = 0; j < i; ++j) if (group[j] == g && relations[i][j] != Relation::Ally) allAllies = false;
            if (allAllies) group[i] = g;
        }
        if (group[i] < 0) group[i] = groups++;
    }
    return groups;
}
struct RamPolicy { bool attack = false, witness = false, assault = false; };
inline RamPolicy RamConsequences(double closing, bool rammerKnown, bool victimKnown, bool playerRammer, bool victimProtected, bool victimHostile)
{
    if (!std::isfinite(closing) || closing < 4.0) return {};
    return {rammerKnown, closing >= 8.0, playerRammer && victimKnown && !victimProtected && !victimHostile};
}
template<int N> struct PairCooldown {
    struct Pair { std::uint32_t a = 0, b = 0; std::uint64_t at = 0; bool used = false; } p[N];
    bool Claim(std::uint32_t a, std::uint32_t b, std::uint64_t now, std::uint64_t cooldown = 1500)
    {
        if (!a || !b || a == b) return false;
        Pair* oldest = &p[0];
        for (auto& pair : p) {
            if (pair.used && ((pair.a == a && pair.b == b) || (pair.a == b && pair.b == a))) {
                if (now >= pair.at && now - pair.at <= cooldown) return false;
                pair = {a, b, now, true}; return true;
            }
            if (!pair.used || (oldest->used && pair.at < oldest->at)) oldest = &pair;
        }
        *oldest = {a, b, now, true}; return true;
    }
};
}

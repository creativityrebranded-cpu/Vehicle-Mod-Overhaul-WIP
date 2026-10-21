#include "combat_relations.h"
#include <cstdio>
#include <limits>
using namespace combatrelations;
int main()
{
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", why); } };
    check(Classify(false, false, 0, 0) == Relation::Neutral, "neutral is not an ally");
    check(Classify(false, false, 2, 3) == Relation::Ally, "reciprocal native ally/friend");
    check(Classify(false, true, 2, 2) == Relation::Enemy, "native combat overrides faction alliance");
    check(Classify(false, false, 2, 1) == Relation::Enemy, "asymmetric hostility separates groups");
    check(Classify(false, false, 2, 0) == Relation::Neutral, "one-way friendship cannot recruit neutral");
    check(Classify(false, false, -1, -1) == Relation::Neutral, "unreadable reaction doesn't create alliance");
    check(Classify(true, true, 1, 1) == Relation::Ally, "player-companion protection remains absolute");
    Relation matrix[6][6] = {};
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) matrix[i][j] = i / 2 == j / 2 ? Relation::Ally : Relation::Enemy;
    int groups[6] = {};
    check(Partition(6, matrix, groups) == 3, "three hostile factions form three squads");
    check(groups[0] == groups[1] && groups[2] == groups[3] && groups[4] == groups[5] && groups[0] != groups[2] && groups[2] != groups[4], "each native faction keeps its allies");
    matrix[0][2] = matrix[2][0] = Relation::Ally;
    Partition(6, matrix, groups);
    check(groups[0] != groups[2], "shared ally does not bridge hostile members");
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) matrix[i][j] = i == j ? Relation::Ally : Relation::Neutral;
    check(Partition(6, matrix, groups) == 6, "six neutrals remain separate even if same profile/enemy");
    PairCooldown<4> cooldown;
    check(cooldown.Claim(1, 2, 0), "first pair at zero accepted");
    check(!cooldown.Claim(2, 1, 1000), "reverse-order same collision is suppressed");
    check(!cooldown.Claim(1, 2, 1500), "cooldown includes boundary");
    check(cooldown.Claim(1, 2, 1501), "new ram after cooldown accepted");
    check(cooldown.Claim(1, 3, 1510), "different victim independent");
    check(!cooldown.Claim(1, 1, 1520) && !cooldown.Claim(0, 2, 1520), "invalid collision ignored");
    auto p = RamConsequences(12, true, true, true, false, false);
    check(p.attack && p.witness && p.assault, "player hard-ram neutral triggers combat witness assault");
    p = RamConsequences(5, true, true, true, false, false);
    check(p.attack && !p.witness && p.assault, "minor damaging ram still attacks but no hard-crash witness");
    p = RamConsequences(12, true, true, true, false, true);
    check(p.attack && p.witness && !p.assault, "already hostile victim does not add neutral assault");
    p = RamConsequences(12, true, true, true, true, false);
    check(!p.assault, "protected companion doesn't create assault alarm");
    p = RamConsequences(12, true, true, false, false, false);
    check(p.attack && p.witness && !p.assault, "NPC ram not charged to player");
    p = RamConsequences(12, false, false, false, false, false);
    check(!p.attack && p.witness && !p.assault, "unattributed collision has witnesses but no invented attacker/crime");
    p = RamConsequences(3.9, true, true, true, false, false);
    check(!p.attack && !p.witness && !p.assault, "parking touch has no attack events");
    p = RamConsequences(std::numeric_limits<double>::quiet_NaN(), true, true, true, false, false);
    check(!p.attack && !p.witness && !p.assault, "invalid speed creates no events");
    std::printf("combat_relations_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

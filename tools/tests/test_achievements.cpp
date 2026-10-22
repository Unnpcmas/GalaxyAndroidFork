// Host test for platform/mobile/achievements.h:
//   g++ -std=c++17 -Iplatform/mobile tools/tests/test_achievements.cpp -o /tmp/test_achievements && /tmp/test_achievements
#include <assert.h>
#include <stdio.h>
#include "achievements.h"

using namespace achv;

static size_t indexOf(const char* id) {
    for (size_t i = 0; i < defs().size(); i++)
        if (!strcmp(defs()[i].id, id)) return i;
    assert(false);
    return 0;
}

int main() {
    Tracker t;
    // Stars already owned unlock retroactively; the first poll is not "progress".
    t.onPoll(1, 30, 120, 0, 100, 10, 3);
    t.evaluate(0, true, 1000);
    assert(t.isUnlocked(indexOf("stars_25")) && !t.isUnlocked(indexOf("stars_50")));
    t.onPoll(1, 30, 120, 0, 150, 70, 4);  // +50 bits, +60 coins, +1 life
    t.evaluate(0, true, 1001);
    assert(t.isUnlocked(indexOf("coins_50")) && t.isUnlocked(indexOf("oneup_1")) && !t.isUnlocked(indexOf("bits_100")));
    // Another save file does not count its totals as progress.
    t.onPoll(2, 30, 120, 0, 5000, 900, 99);
    t.evaluate(0, true, 1002);
    assert(!t.isUnlocked(indexOf("coins_200")) && !t.isUnlocked(indexOf("oneup_10")));
    // Bosses.
    assert(t.onStarRequested("KoopaVs2") == "boss:bowser2");
    assert(t.onStarRequested("DinoPackunVs2") == "boss:dino2");
    assert(t.onStarRequested("DinoPackun") == "boss:dino1");
    assert(t.onStarRequested("TripodBossCore") == "boss:megaleg");
    assert(t.onStarRequested("Unknown").empty());
    assert(t.onStarRequested("Unknown", "EggStarGalaxy").empty());
    assert(t.onStarRequested("whatever", "TriLegLv1Galaxy") == "boss:megaleg");  // stage fallback
    {   // reloading a save / game over must not count as 1-ups
        Tracker u;
        u.onStage("EggStarGalaxy", false);
        u.onPoll(1, 0, 120, 0, 0, 0, 4);
        u.onPoll(1, 0, 120, 0, 0, 0, 5);
        u.onStage("EggStarGalaxy", false);       // new session re-baselines
        u.onPoll(1, 0, 120, 0, 0, 0, 4);
        u.onPoll(1, 0, 120, 0, 0, 0, 4 + 9);     // implausible jump ignored
        for (const Def& d : defs()) if (d.metric == M_ONE_UPS) { assert(u.metricValue(d, 0, true) == 1); break; }
    }
    assert(t.evaluate(0, true, 1002).size() == 4);  // the four bosses
    // Per-achievement reset: progress restarts from zero.
    const size_t coins50 = indexOf("coins_50");
    t.resetOne(coins50, 0, true);
    assert(!t.isUnlocked(coins50));
    assert(t.evaluate(0, true, 1003).empty());
    t.onPoll(2, 30, 120, 0, 5000, 949, 99);
    assert(t.evaluate(0, true, 1004).empty());
    t.onPoll(2, 30, 120, 0, 5000, 951, 99);
    assert(t.isUnlocked(coins50) == false);
    t.evaluate(0, true, 1005);
    assert(t.isUnlocked(coins50));
    // Persistence round trip.
    FILE* f = fopen("/tmp/test_achievements.ini", "w");
    t.saveLines(f);
    fclose(f);
    Tracker u;
    f = fopen("/tmp/test_achievements.ini", "r");
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* e = strchr(line, '=');
        *e++ = 0;
        char* n = strpbrk(e, "\r\n");
        if (n) *n = 0;
        assert(u.loadLine(line, e));
    }
    fclose(f);
    assert(u.unlockedCount() == t.unlockedCount());
    // Everything unlocked -> platinum; resetting it needs another achievement re-earned.
    Tracker q;
    q.onPoll(1, 240, 240, 6, 0, 0, 0);
    for (const BossMatch& m : bossMatches()) q.onStarRequested(m.object);
    q.onEnding(true, true);
    q.onStage("AstroGalaxy", true);
    const char* stages[] = {"EggStarGalaxy", "HeavenlyBeachGalaxy", "HoneyBeeKingdomGalaxy", "CosmosGardenGalaxy",
                            "IceVolcanoGalaxy", "SandClockGalaxy", "FactoryGalaxy", "HellProminenceGalaxy",
                            "PhantomGalaxy", "StarDustGalaxy", "BattleShipGalaxy", "OceanRingGalaxy", "FishTunnelGalaxy",
                            "TearDropGalaxy", "SnowCapsuleGalaxy", "BeltConveyerExGalaxy", "CocoonExGalaxy",
                            "FlipPanelExGalaxy", "DarkRoomGalaxy", "SurfingLv1Galaxy", "CubeBubbleExLv1Galaxy",
                            "TamakoroExLv1Galaxy", "TriLegLv1Galaxy", "KoopaBattleVs1Galaxy", "KoopaBattleVs2Galaxy",
                            "KoopaBattleVs3Galaxy", "FloaterOtaKingGalaxy", "SkullSharkGalaxy", "OceanPhantomCaveGalaxy",
                            "BreakDownPlanetGalaxy", "TransformationExGalaxy"};
    for (const char* s : stages) q.onStage(s, false);
    for (int i = 0; i < 6; i++) q.onComet();
    for (int i = 0; i < 40; i++) {
        q.onPoll(1, 240, 240, 6, 10000 + i * 500, 5000 + i * 100, 50 + i);
        q.onDeath();
    }
    for (int d = 1; d <= 8; d++) q.onDayPlayed(20261000 + d);
    q.evaluate(200000, true, 5000);
    assert((size_t)q.unlockedCount() == defs().size());
    const size_t plat = indexOf("all_achievements");
    q.resetOne(plat, 200000, true);
    assert(q.evaluate(200000, true, 5001).empty());
    q.resetOne(indexOf("coins_50"), 200000, true);
    q.evaluate(200000, true, 5002);
    assert(!q.isUnlocked(plat));
    q.onPoll(1, 240, 240, 6, 10000, 5000 + 40 * 100 + 60, 90);
    q.evaluate(200000, true, 5003);
    assert(q.isUnlocked(plat));
    printf("achievements: %zu defined, all checks passed\n", defs().size());
    return 0;
}

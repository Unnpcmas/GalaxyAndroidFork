// Host test for the randomizer engine: g++ -std=c++17 -Wall -Iplatform/include tools/tests/test_randomizer.cpp
#include <assert.h>
#include <stdio.h>
#include <set>
#include <string>

#include "port/randomizer.h"

using namespace rando;

int main() {
    const unsigned all = F_GALAXIES | F_STARS | F_ENEMIES | F_ITEMS;

    // Off: everything is identity.
    {
        Engine e;
        e.configure(seedFromNumber(1), 0);
        assert(!e.active());
        assert(!strcmp(e.galaxy("EggStarGalaxy"), "EggStarGalaxy"));
        assert(e.scenario("EggStarGalaxy", 3, 6) == 3);
        assert(!strcmp(e.object("Kuribo"), "Kuribo"));
    }

    for (unsigned n : {0u, 1u, 123456u, 999999u}) {
        Engine e;
        e.configure(seedFromNumber(n), all);
        assert(e.active());
        // Galaxies: a bijection over the pool with no door leading to itself.
        std::set<std::string> seen;
        for (const char* g : galaxyPool()) {
            const char* t = e.galaxy(g);
            assert(strcmp(t, g) != 0);
            assert(inGalaxyPool(t));
            seen.insert(t);
        }
        assert(seen.size() == galaxyPool().size());
        // Non-pool stages are untouched (boss / hub galaxies).
        assert(!strcmp(e.galaxy("KoopaBattleVs1Galaxy"), "KoopaBattleVs1Galaxy"));
        assert(!strcmp(e.galaxy("AstroGalaxy"), "AstroGalaxy"));
        // Missions: a bijection of 1..k, none stays, outside values untouched.
        for (int k = 2; k <= 8; k++) {
            std::set<int> got;
            for (int s = 1; s <= k; s++) {
                const int t = e.scenario("PhantomGalaxy", s, k);
                assert(t >= 1 && t <= k && t != s);
                got.insert(t);
            }
            assert((int)got.size() == k);
        }
        assert(e.scenario("PhantomGalaxy", 9, 6) == 9);   // not a normal mission
        assert(e.scenario("PhantomGalaxy", 1, 1) == 1);   // one mission: nothing to shuffle
        assert(e.scenario("KoopaBattleVs1Galaxy", 1, 3) == 1);
        // Objects: families are closed under the mapping.
        assert(!strcmp(e.object("Kuribo"), "BombHei") && !strcmp(e.object("BombHei"), "Kuribo"));
        assert(!strcmp(e.object("Teresa"), "BombTeresa"));
        assert(!strcmp(e.object("KinokoOneUp"), "KinokoLifeUp"));
        // Power-ups are never touched unless F_POWERUPS is set.
        for (const char* p : powerUpFamilies()[0]) assert(!strcmp(e.object(p), p));
        assert(!strcmp(e.object("Coin"), "Coin"));
    }
    {
        Engine e;
        e.configure(seedFromNumber(5), F_ITEMS | F_POWERUPS);
        std::set<std::string> pu;
        for (const char* p : powerUpFamilies()[0]) {
            assert(strcmp(e.object(p), p) != 0);
            pu.insert(e.object(p));
        }
        assert(pu.size() == powerUpFamilies()[0].size());
        // Challenge mode wins over the power-up switch.
        e.configure(seedFromNumber(5), F_ITEMS | F_POWERUPS | F_CHALLENGE);
        for (const char* p : powerUpFamilies()[0]) assert(!strcmp(e.object(p), p));
    }

    // Deterministic, and different seeds differ somewhere.
    {
        Engine a, b, c;
        a.configure(seedFromNumber(42), all);
        b.configure(seedFromNumber(42), all);
        c.configure(seedFromNumber(43), all);
        bool differs = false;
        for (const char* g : galaxyPool()) {
            assert(!strcmp(a.galaxy(g), b.galaxy(g)));
            if (strcmp(a.galaxy(g), c.galaxy(g)) != 0) differs = true;
        }
        assert(differs);
        // Re-configuring with the same values keeps the tables.
        const std::string before = a.galaxy("EggStarGalaxy");
        a.configure(seedFromNumber(42), all);
        assert(before == a.galaxy("EggStarGalaxy"));
        // Changing a flag rebuilds: galaxies off -> identity again.
        a.configure(seedFromNumber(42), F_ENEMIES);
        assert(!strcmp(a.galaxy("EggStarGalaxy"), "EggStarGalaxy"));
        assert(a.scenario("EggStarGalaxy", 2, 5) == 2);
    }

    // Challenge mode leaves the power-ups alone but still shuffles the rest.
    {
        Engine e;
        e.configure(seedFromNumber(7), all | F_POWERUPS | F_CHALLENGE);
        for (const char* p : powerUpFamilies()[0]) assert(!strcmp(e.object(p), p));
        assert(!strcmp(e.object("Kuribo"), "BombHei"));
        assert(strcmp(e.galaxy("EggStarGalaxy"), "EggStarGalaxy") != 0);
    }

    printf("randomizer: all checks passed\n");
    return 0;
}

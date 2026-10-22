// Achievements: definitions, progress tracking and persistence.
//
// Header-only and independent of the UI so it can be tested on a host
// (tools/tests/test_achievements.cpp). The game reports facts through the
// on*() calls; evaluate() turns them into unlocks.
//
// Every achievement reads one metric:
//   - cumulative metrics (coins, star bits, 1-ups, deaths, play time, boss
//     defeats, visits...) grow while playing; "reset" stores the value as the
//     achievement's base, so its progress starts again from zero;
//   - state metrics (stars owned, galaxies visited, days played) describe how
//     far the player is; after a "reset" they must grow past the stored base
//     before the achievement unlocks again.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The tracker (its map, vectors and strings) lives for the whole run but is called from
// emulated game threads, where plain operator new allocates inside the game's JKRHeap.
// That heap is reset on scene changes, so nodes allocated there get trampled and a later
// lookup panics in JKRExpHeap::do_alloc.  Every entry point therefore allocates from the
// host heap.  (Weak symbols: the host test links without the port.)
extern "C" {
void port_host_alloc_scope_enter(void) __attribute__((weak));
void port_host_alloc_scope_leave(void) __attribute__((weak));
}
namespace achv {
struct HostHeapScope {
    HostHeapScope() { if (port_host_alloc_scope_enter) port_host_alloc_scope_enter(); }
    ~HostHeapScope() { if (port_host_alloc_scope_leave) port_host_alloc_scope_leave(); }
    HostHeapScope(const HostHeapScope&) = delete;
    HostHeapScope& operator=(const HostHeapScope&) = delete;
};
}  // namespace achv

namespace achv {

enum Metric : int {
    M_FIRST_GAME,   // a gameplay scene was started once
    M_STARS,        // Power Stars owned (best value seen)
    M_GRAND_STARS,  // Grand Stars owned
    M_STAR_BITS,    // star bits picked up (cumulative)
    M_COINS,        // coins picked up (cumulative)
    M_ONE_UPS,      // extra lives picked up (cumulative)
    M_DEATHS,       // lives lost (cumulative)
    M_PLAY_SECONDS, // play time (cumulative, supplied by the caller)
    M_GALAXIES,     // different galaxies entered
    M_DAYS,         // different days played
    M_KEY,          // count stored under Def::key (visits, boss defeats, ...)
    M_COMETS,       // comet galaxies entered (cumulative)
    M_FULL_STARS,   // every Power Star of the game
    M_ALL,          // every other achievement
};

struct Def {
    const char* id;
    const char* es;
    const char* en;
    const char* descEs;
    const char* descEn;
    Metric metric;
    const char* key;      // for M_KEY
    uint64_t target;
    bool cumulative;
};

// --- Boss objects (names the game gives to the object that raises the star) ---
struct BossMatch {
    const char* object;  // exact object name, or a prefix when prefix is true
    bool prefix;
    const char* key;     // "boss:<key>"
};

inline const std::vector<BossMatch>& bossMatches() {
    HostHeapScope hostHeap;
    static const std::vector<BossMatch> v = {
        {"DinoPackunVs2", false, "boss:dino2"},
        {"DinoPackun", false, "boss:dino1"},
        {"OtaKing", false, "boss:kaliente"},
        {"BossStinkBug", false, "boss:bugaboom"},
        {"StinkBugParent", false, "boss:mandibug"},
        {"TripodBoss", true, "boss:megaleg"},
        {"TriPodBoss", true, "boss:megaleg"},
        {"BossKameck2", false, "boss:kamella2"},
        {"BossKameck", false, "boss:kamella1"},
        {"TombSpider", false, "boss:tarantox"},
        {"BossBegoman", false, "boss:topmaniac"},
        {"KoopaVs1", false, "boss:bowser1"},
        {"KoopaVs2", false, "boss:bowser2"},
        {"KoopaVs3", false, "boss:bowser3"},
        {"Polta", false, "boss:bouldergeist"},
        {"KoopaJr", true, "boss:bowserjr"},
        {"IceMerameraKing", false, "boss:baronbrrr"},
        {"SkeletalFishBoss", false, "boss:kingfin"},
        {"Dodoryu", false, "boss:burrows"},
    };
    return v;
}

// Dedicated boss galaxies: if the object name did not match (instance names
// are unverified), a Power Star raised inside one of these still credits the boss.
struct StageBoss { const char* stage; const char* key; };
inline const StageBoss* stageBosses() {
    static const StageBoss t[] = {
        {"KoopaBattleVs1Galaxy", "boss:bowser1"}, {"KoopaBattleVs2Galaxy", "boss:bowser2"},
        {"KoopaBattleVs3Galaxy", "boss:bowser3"}, {"TriLegLv1Galaxy", "boss:megaleg"},
        {"FloaterOtaKingGalaxy", "boss:kaliente"}, {"KoopaJrShipLv1Galaxy", "boss:bowserjr"},
        {nullptr, nullptr}};
    return t;
}

inline const std::vector<Def>& defs() {
    HostHeapScope hostHeap;
    static const std::vector<Def> v = [] {
        std::vector<Def> d;
        auto add = [&](const char* id, const char* es, const char* en, const char* descEs, const char* descEn, Metric m,
                       const char* key, uint64_t target, bool cumulative) {
            d.push_back(Def{id, es, en, descEs, descEn, m, key, target, cumulative});
        };
        add("first_adventure", "Primera aventura", "First Adventure", "Inicia tu primera partida.",
            "Start your first game.", M_FIRST_GAME, "", 1, false);

        // Power Stars
        add("stars_1", "Primera estrella", "First Star", "Consigue tu primera Estrella de Poder.",
            "Get your first Power Star.", M_STARS, "", 1, false);
        add("stars_5", "Cazador de estrellas", "Star Hunter", "Ten 5 Estrellas de Poder.", "Own 5 Power Stars.",
            M_STARS, "", 5, false);
        add("stars_10", "Diez estrellas", "Ten Stars", "Ten 10 Estrellas de Poder.", "Own 10 Power Stars.", M_STARS, "",
            10, false);
        add("stars_25", "Explorador de galaxias", "Galaxy Explorer", "Ten 25 Estrellas de Poder.",
            "Own 25 Power Stars.", M_STARS, "", 25, false);
        add("stars_50", "Medio centenar", "Fifty Stars", "Ten 50 Estrellas de Poder.", "Own 50 Power Stars.", M_STARS,
            "", 50, false);
        add("stars_75", "Tres cuartos de camino", "Three Quarters", "Ten 75 Estrellas de Poder.",
            "Own 75 Power Stars.", M_STARS, "", 75, false);
        add("stars_100", "Maestro de estrellas", "Star Master", "Ten 100 Estrellas de Poder.", "Own 100 Power Stars.",
            M_STARS, "", 100, false);
        add("stars_120", "Las 120 estrellas", "All 120 Stars", "Ten 120 Estrellas de Poder.", "Own 120 Power Stars.",
            M_STARS, "", 120, false);
        add("grand_1", "Gran Estrella", "Grand Star", "Consigue una Gran Estrella.", "Get a Grand Star.", M_GRAND_STARS,
            "", 1, false);
        add("grand_6", "Seis Grandes Estrellas", "Six Grand Stars", "Consigue las 6 Grandes Estrellas.",
            "Get all 6 Grand Stars.", M_GRAND_STARS, "", 6, false);

        // Star bits
        add("bits_100", "Polvo de estrellas", "Stardust", "Recoge 100 fragmentos de estrella.",
            "Collect 100 Star Bits.", M_STAR_BITS, "", 100, true);
        add("bits_500", "Lluvia de fragmentos", "Star Shower", "Recoge 500 fragmentos de estrella.",
            "Collect 500 Star Bits.", M_STAR_BITS, "", 500, true);
        add("bits_1000", "Mil fragmentos", "A Thousand Bits", "Recoge 1000 fragmentos de estrella.",
            "Collect 1000 Star Bits.", M_STAR_BITS, "", 1000, true);
        add("bits_5000", "Generoso con los Destellos", "Luma's Benefactor", "Recoge 5000 fragmentos de estrella.",
            "Collect 5000 Star Bits.", M_STAR_BITS, "", 5000, true);

        // Coins
        add("coins_50", "Monedero ligero", "Pocket Change", "Recoge 50 monedas.", "Collect 50 coins.", M_COINS, "", 50,
            true);
        add("coins_200", "Bolsillos llenos", "Full Pockets", "Recoge 200 monedas.", "Collect 200 coins.", M_COINS, "",
            200, true);
        add("coins_500", "Avaricia galactica", "Galactic Greed", "Recoge 500 monedas.", "Collect 500 coins.", M_COINS,
            "", 500, true);
        add("coins_1000", "Rico del espacio", "Space Tycoon", "Recoge 1000 monedas.", "Collect 1000 coins.", M_COINS,
            "", 1000, true);

        // 1-Ups
        add("oneup_1", "Una vida extra", "Extra Life", "Recoge un Champinon 1UP.", "Pick up a 1-Up Mushroom.",
            M_ONE_UPS, "", 1, true);
        add("oneup_10", "Vidas de sobra", "Lives to Spare", "Gana 10 vidas extra.", "Gain 10 extra lives.", M_ONE_UPS,
            "", 10, true);
        add("oneup_30", "Siete vidas, y mas", "Nine Lives", "Gana 30 vidas extra.", "Gain 30 extra lives.", M_ONE_UPS,
            "", 30, true);

        // Deaths
        add("death_1", "Primer tropiezo", "First Stumble", "Pierde una vida.", "Lose a life.", M_DEATHS, "", 1, true);
        add("death_10", "Aprendiendo a golpes", "Learning the Hard Way", "Pierde 10 vidas.", "Lose 10 lives.", M_DEATHS,
            "", 10, true);
        add("death_30", "Terco como Bowser", "Stubborn as Bowser", "Pierde 30 vidas y sigue intentandolo.",
            "Lose 30 lives and keep trying.", M_DEATHS, "", 30, true);

        // Play time
        add("time_1h", "Primera hora", "First Hour", "Juega 1 hora.", "Play for 1 hour.", M_PLAY_SECONDS, "", 3600,
            true);
        add("time_5h", "Viaje largo", "Long Trip", "Juega 5 horas.", "Play for 5 hours.", M_PLAY_SECONDS, "", 18000,
            true);
        add("time_10h", "Tripulante del Observatorio", "Observatory Crew", "Juega 10 horas.", "Play for 10 hours.",
            M_PLAY_SECONDS, "", 36000, true);
        add("time_25h", "Leyenda galactica", "Galactic Legend", "Juega 25 horas.", "Play for 25 hours.",
            M_PLAY_SECONDS, "", 90000, true);

        // Galaxies entered
        add("galaxies_3", "Turista espacial", "Space Tourist", "Entra en 3 galaxias distintas.",
            "Enter 3 different galaxies.", M_GALAXIES, "", 3, false);
        add("galaxies_10", "Viajero estelar", "Star Traveler", "Entra en 10 galaxias distintas.",
            "Enter 10 different galaxies.", M_GALAXIES, "", 10, false);
        add("galaxies_20", "Trotamundos", "Globetrotter", "Entra en 20 galaxias distintas.",
            "Enter 20 different galaxies.", M_GALAXIES, "", 20, false);
        add("galaxies_30", "Conocedor del cosmos", "Cosmos Connoisseur", "Entra en 30 galaxias distintas.",
            "Enter 30 different galaxies.", M_GALAXIES, "", 30, false);

        // Days played
        add("days_3", "Regreso constante", "Coming Back", "Juega en 3 dias distintos.", "Play on 3 different days.",
            M_DAYS, "", 3, false);
        add("days_7", "Rutina estelar", "Stellar Routine", "Juega en 7 dias distintos.", "Play on 7 different days.",
            M_DAYS, "", 7, false);

        // Places of the game
        add("visit_astro", "Hogar en el Observatorio", "Home at the Observatory",
            "Entra en el Observatorio del Cometa.", "Enter the Comet Observatory.", M_KEY, "v:AstroGalaxy", 1, true);
        add("visit_egg", "Bienvenido a Good Egg", "Welcome to Good Egg", "Entra en Good Egg Galaxy.",
            "Enter Good Egg Galaxy.", M_KEY, "v:EggStarGalaxy", 1, true);
        add("visit_beach", "Dia de playa", "Beach Day", "Entra en Beach Bowl Galaxy.", "Enter Beach Bowl Galaxy.",
            M_KEY, "v:HeavenlyBeachGalaxy", 1, true);
        add("visit_honeybee", "Zumbido en la colmena", "Buzz in the Hive", "Entra en Honeyhive Galaxy.",
            "Enter Honeyhive Galaxy.", M_KEY, "v:HoneyBeeKingdomGalaxy", 1, true);
        add("visit_garden", "Jardin ventoso", "Gusty Garden", "Entra en Gusty Garden Galaxy.",
            "Enter Gusty Garden Galaxy.", M_KEY, "v:CosmosGardenGalaxy", 1, true);
        add("visit_ice", "Fuego y hielo", "Fire and Ice", "Entra en Freezeflame Galaxy.", "Enter Freezeflame Galaxy.",
            M_KEY, "v:IceVolcanoGalaxy", 1, true);
        add("visit_dune", "Entre las dunas", "Among the Dunes", "Entra en Dusty Dune Galaxy.",
            "Enter Dusty Dune Galaxy.", M_KEY, "v:SandClockGalaxy", 1, true);
        add("visit_toy", "Hora de jugar", "Toy Time", "Entra en Toy Time Galaxy.", "Enter Toy Time Galaxy.", M_KEY,
            "v:FactoryGalaxy", 1, true);
        add("visit_lava", "Calor extremo", "Extreme Heat", "Entra en Melty Molten Galaxy.",
            "Enter Melty Molten Galaxy.", M_KEY, "v:HellProminenceGalaxy", 1, true);
        add("visit_ghost", "Escalofrios", "Chills", "Entra en Ghostly Galaxy.", "Enter Ghostly Galaxy.", M_KEY,
            "v:PhantomGalaxy", 1, true);
        add("visit_junk", "Chatarra espacial", "Space Junk", "Entra en Space Junk Galaxy.",
            "Enter Space Junk Galaxy.", M_KEY, "v:StarDustGalaxy", 1, true);
        add("visit_battlerock", "Roca de batalla", "Battlerock", "Entra en Battlerock Galaxy.",
            "Enter Battlerock Galaxy.", M_KEY, "v:BattleShipGalaxy", 1, true);

        // Comets and heroes
        add("comet_1", "Cometa a la vista", "Comet Sighted", "Entra en una galaxia con cometa.",
            "Enter a galaxy with a comet.", M_COMETS, "", 1, true);
        add("comet_5", "Cazador de cometas", "Comet Chaser", "Entra en 5 galaxias con cometa.",
            "Enter 5 comet galaxies.", M_COMETS, "", 5, true);
        add("luigi", "El hermano alto", "The Tall Brother", "Juega una partida como Luigi.",
            "Play a game as Luigi.", M_KEY, "luigi", 1, true);
        add("ending", "Final de la aventura", "The Adventure Ends", "Termina la historia del juego.",
            "Finish the game's story.", M_KEY, "ending", 1, true);
        add("rando_ending", "Mundo al azar", "Shuffled World", "Termina la historia con el randomizer activado.",
            "Finish the story with the randomizer on.", M_KEY, "rando_ending", 1, true);
        add("rando_challenge_ending", "Desafio completado", "Challenge Cleared",
            "Termina la historia con el randomizer en modo desafio.",
            "Finish the story with the randomizer in challenge mode.", M_KEY, "rando_challenge_ending", 1, true);

        // Bosses
        add("boss_dino1", "Dino Piranha derrotado", "Dino Piranha Defeated", "Derrota a Dino Piranha.",
            "Defeat Dino Piranha.", M_KEY, "boss:dino1", 1, true);
        add("boss_dino2", "Dino Piranha: revancha", "Dino Piranha: Rematch", "Derrota a Dino Piranha por segunda vez.",
            "Defeat Dino Piranha's second fight.", M_KEY, "boss:dino2", 1, true);
        add("boss_kaliente", "Rey Kaliente derrotado", "King Kaliente Defeated", "Derrota a King Kaliente.",
            "Defeat King Kaliente.", M_KEY, "boss:kaliente", 1, true);
        add("boss_bugaboom", "Bugaboom derrotado", "Bugaboom Defeated", "Derrota a Bugaboom.", "Defeat Bugaboom.",
            M_KEY, "boss:bugaboom", 1, true);
        add("boss_mandibug", "Pila de Mandibugs derrotada", "Mandibug Stack Defeated",
            "Derrota a la pila de Mandibugs.", "Defeat the Mandibug Stack.", M_KEY, "boss:mandibug", 1, true);
        add("boss_megaleg", "Megaleg derrotado", "Megaleg Defeated", "Derrota a Megaleg.", "Defeat Megaleg.", M_KEY,
            "boss:megaleg", 1, true);
        add("boss_kamella1", "Kamella derrotada", "Kamella Defeated", "Derrota a Kamella.", "Defeat Kamella.", M_KEY,
            "boss:kamella1", 1, true);
        add("boss_kamella2", "Kamella: revancha", "Kamella: Rematch", "Derrota a Kamella por segunda vez.",
            "Defeat Kamella's second fight.", M_KEY, "boss:kamella2", 1, true);
        add("boss_tarantox", "Tarantox derrotado", "Tarantox Defeated", "Derrota a Tarantox.", "Defeat Tarantox.",
            M_KEY, "boss:tarantox", 1, true);
        add("boss_topmaniac", "Topmaniac derrotado", "Topmaniac Defeated", "Derrota a Topmaniac.",
            "Defeat Topmaniac.", M_KEY, "boss:topmaniac", 1, true);
        add("boss_bowser1", "Bowser: primer combate", "Bowser: First Fight", "Derrota a Bowser por primera vez.",
            "Defeat Bowser for the first time.", M_KEY, "boss:bowser1", 1, true);
        add("boss_bowser2", "Bowser: segundo combate", "Bowser: Second Fight", "Derrota a Bowser por segunda vez.",
            "Defeat Bowser for the second time.", M_KEY, "boss:bowser2", 1, true);
        add("boss_bowser3", "Bowser: combate final", "Bowser: Final Fight", "Derrota a Bowser en el combate final.",
            "Defeat Bowser in the final fight.", M_KEY, "boss:bowser3", 1, true);
        add("boss_bouldergeist", "Bouldergeist derrotado", "Bouldergeist Defeated", "Derrota a Bouldergeist.",
            "Defeat Bouldergeist.", M_KEY, "boss:bouldergeist", 1, true);
        add("boss_bowserjr", "Bowser Jr. derrotado", "Bowser Jr. Defeated", "Derrota a Bowser Jr.",
            "Defeat Bowser Jr.", M_KEY, "boss:bowserjr", 1, true);
        add("boss_baronbrrr", "Baron Brrr derrotado", "Baron Brrr Defeated", "Derrota a Baron Brrr.",
            "Defeat Baron Brrr.", M_KEY, "boss:baronbrrr", 1, true);
        add("boss_kingfin", "Kingfin derrotado", "Kingfin Defeated", "Derrota a Kingfin.", "Defeat Kingfin.", M_KEY,
            "boss:kingfin", 1, true);
        add("boss_burrows", "Major Burrows derrotado", "Major Burrows Defeated", "Derrota a Major Burrows.",
            "Defeat Major Burrows.", M_KEY, "boss:burrows", 1, true);

        // Completion
        add("full_game", "100% del juego", "100% Complete", "Consigue todas las Estrellas de Poder del juego.",
            "Get every Power Star in the game.", M_FULL_STARS, "", 1, false);
        add("all_achievements", "Platino galactico", "Galactic Platinum", "Desbloquea todos los demas logros.",
            "Unlock every other achievement.", M_ALL, "", 1, false);
        return d;
    }();
    return v;
}

// Galaxy names that count as "a galaxy entered" (not the hidden/mini/comet variants).
inline bool isCountedGalaxy(const char* stage) {
    if (!stage) return false;
    const size_t n = strlen(stage);
    if (n < 6 || strcmp(stage + n - 6, "Galaxy") != 0) return false;
    static const char* const kSkip[] = {"Mini",        "Appear",  "Surp",      "StarPiece", "ExOpen",
                                        "Special",     "StarNum", "NormalComet", "Coin100Comet", "E3"};
    for (const char* skip : kSkip) {
        if (strncmp(stage, skip, strlen(skip)) == 0) return false;
    }
    return true;
}

inline bool isCometStage(const char* stage) {
    return stage && (strncmp(stage, "NormalComet", 11) == 0 || strncmp(stage, "Coin100Comet", 12) == 0);
}

struct State {
    int64_t unlockedAt = 0;  // epoch seconds, 0 = locked
    uint64_t base = 0;       // value at the last reset (see header comment)
};

struct Progress {
    uint64_t current = 0;  // progress towards target (already relative to the base)
    uint64_t target = 1;
    bool unlocked = false;
    int64_t unlockedAt = 0;
};

class Tracker {
public:
    Tracker() {
        HostHeapScope hostHeap;
        states_.assign(defs().size(), State());
    }

    size_t count() const { return defs().size(); }

    // ---- Facts reported by the game -------------------------------------
    // Called about once a second while a gameplay scene runs.
    void onPoll(int userFile, int stars, int starMax, int grandStars, int starBits, int coins, int lives) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stars > 0 && (uint64_t)stars > maxStars_) maxStars_ = (uint64_t)stars;
        if (starMax > 0) starMax_ = (uint64_t)starMax;
        if (grandStars > 0 && (uint64_t)grandStars > maxGrand_) maxGrand_ = (uint64_t)grandStars;
        if (!pollInit_ || userFile != lastUserFile_) {
            // A different save file: its totals are not progress made now.
            pollInit_ = true;
            lastUserFile_ = userFile;
        } else {
            if (starBits > lastBits_) cum_[0] += (uint64_t)(starBits - lastBits_);
            if (coins > lastCoins_) cum_[1] += (uint64_t)(coins - lastCoins_);
            if (lives > lastLives_ && lives - lastLives_ <= 5) cum_[2] += (uint64_t)(lives - lastLives_);
        }
        lastBits_ = starBits;
        lastCoins_ = coins;
        lastLives_ = lives;
    }

    void onStage(const char* stage, bool luigi) {
        if (!stage || !*stage) return;
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        keyCounts_[std::string("v:") + stage]++;
        if (luigi) keyCounts_["luigi"]++;
        pollInit_ = false;  // new gameplay session: re-baseline bits/coins/lives (no false 1-ups on reload)
    }

    // A comet scenario was entered (reported by the game's CometEventKeeper).
    void onComet() {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        cum_[4]++;
    }

    void onDeath() {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        cum_[3]++;
    }

    // `randomizer` / `challenge`: the randomizer (and its challenge mode) was on at the ending.
    void onEnding(bool randomizer = false, bool challenge = false) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        keyCounts_["ending"]++;
        if (randomizer) keyCounts_["rando_ending"]++;
        if (randomizer && challenge) keyCounts_["rando_challenge_ending"]++;
    }

    // Returns the boss key that was matched ("" when the object is not a boss).
    std::string onStarRequested(const char* objectName, const char* stage = nullptr) {
        if (!objectName) objectName = "";
        for (const BossMatch& m : bossMatches()) {
            const bool hit = m.prefix ? strncmp(objectName, m.object, strlen(m.object)) == 0
                                      : strcmp(objectName, m.object) == 0;
            if (hit) {
                HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
                keyCounts_[m.key]++;
                return m.key;
            }
        }
        if (stage) {
            for (const StageBoss* b = stageBosses(); b->stage; b++) {
                if (strcmp(stage, b->stage) == 0) {
                    HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
                    keyCounts_[b->key]++;
                    return b->key;
                }
            }
        }
        return std::string();
    }

    // yyyymmdd of today; counts a new day played.
    void onDayPlayed(int yyyymmdd) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        if (yyyymmdd != lastDay_) {
            lastDay_ = yyyymmdd;
            days_++;
        }
    }

    // ---- Evaluation -------------------------------------------------------
    // Returns the indices that unlocked now.
    std::vector<int> evaluate(uint64_t playSeconds, bool firstGame, int64_t nowEpoch) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int> fresh;
        const std::vector<Def>& d = defs();
        for (size_t i = 0; i < d.size(); i++) {
            if (d[i].metric == M_ALL || states_[i].unlockedAt != 0) continue;
            if (met(d[i], valueLocked(d[i], playSeconds, firstGame), states_[i])) {
                states_[i].unlockedAt = nowEpoch > 0 ? nowEpoch : 1;
                fresh.push_back((int)i);
            }
        }
        for (size_t i = 0; i < d.size(); i++) {
            if (d[i].metric != M_ALL) continue;
            const bool all = allOthersUnlockedLocked();
            if (states_[i].base == 1 && !all) states_[i].base = 0;  // re-armed by a reset elsewhere
            if (states_[i].unlockedAt == 0 && all && states_[i].base == 0) {
                states_[i].unlockedAt = nowEpoch > 0 ? nowEpoch : 1;
                fresh.push_back((int)i);
            }
        }
        return fresh;
    }

    Progress progress(size_t i, uint64_t playSeconds, bool firstGame) const {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        Progress p;
        const Def& d = defs()[i];
        p.unlocked = states_[i].unlockedAt != 0;
        p.unlockedAt = states_[i].unlockedAt;
        p.target = d.target ? d.target : 1;
        if (d.metric == M_ALL) {
            p.target = defs().size() - 1;
            p.current = unlockedCountLocked(true);
            return p;
        }
        const uint64_t v = valueLocked(d, playSeconds, firstGame);
        if (d.cumulative && d.metric != M_FIRST_GAME) {
            p.current = v > states_[i].base ? v - states_[i].base : 0;
        } else {
            p.current = v;
        }
        if (p.current > p.target) p.current = p.target;
        if (p.unlocked) p.current = p.target;
        return p;
    }

    int unlockedCount() const {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        return (int)unlockedCountLocked(false);
    }

    bool isUnlocked(size_t i) const {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        return i < states_.size() && states_[i].unlockedAt != 0;
    }

    // ---- Reset ----------------------------------------------------------------
    void resetOne(size_t i, uint64_t playSeconds, bool firstGame) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        resetLocked(i, playSeconds, firstGame);
    }

    void resetAll(uint64_t playSeconds, bool firstGame) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < states_.size(); i++) resetLocked(i, playSeconds, firstGame);
    }

    // ---- Persistence ----------------------------------------------------------
    void saveLines(FILE* f) const {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::vector<Def>& d = defs();
        for (size_t i = 0; i < d.size(); i++) {
            if (states_[i].unlockedAt != 0 || states_[i].base != 0) {
                fprintf(f, "ach.%s=%lld:%llu\n", d[i].id, (long long)states_[i].unlockedAt,
                        (unsigned long long)states_[i].base);
            }
        }
        fprintf(f, "cnt.bits=%llu\ncnt.coins=%llu\ncnt.oneups=%llu\ncnt.deaths=%llu\ncnt.comets=%llu\n",
                (unsigned long long)cum_[0], (unsigned long long)cum_[1], (unsigned long long)cum_[2],
                (unsigned long long)cum_[3], (unsigned long long)cum_[4]);
        fprintf(f, "max.stars=%llu\nmax.starmax=%llu\nmax.grand=%llu\ndays=%llu\nlastday=%d\n",
                (unsigned long long)maxStars_, (unsigned long long)starMax_, (unsigned long long)maxGrand_,
                (unsigned long long)days_, lastDay_);
        for (const auto& kv : keyCounts_) {
            fprintf(f, "key.%s=%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
        }
    }

    // Returns true when the line belonged to the tracker.
    bool loadLine(const char* key, const char* value) {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t n = strtoull(value, nullptr, 10);
        if (!strncmp(key, "ach.", 4)) {
            const std::vector<Def>& d = defs();
            for (size_t i = 0; i < d.size(); i++) {
                if (!strcmp(key + 4, d[i].id)) {
                    const char* colon = strchr(value, ':');
                    states_[i].unlockedAt = strtoll(value, nullptr, 10);
                    states_[i].base = colon ? strtoull(colon + 1, nullptr, 10) : 0;
                    break;
                }
            }
            return true;
        }
        if (!strncmp(key, "key.", 4)) {
            keyCounts_[key + 4] = n;
            return true;
        }
        if (!strcmp(key, "cnt.bits")) cum_[0] = n;
        else if (!strcmp(key, "cnt.coins")) cum_[1] = n;
        else if (!strcmp(key, "cnt.oneups")) cum_[2] = n;
        else if (!strcmp(key, "cnt.deaths")) cum_[3] = n;
        else if (!strcmp(key, "cnt.comets")) cum_[4] = n;
        else if (!strcmp(key, "max.stars")) maxStars_ = n;
        else if (!strcmp(key, "max.starmax")) starMax_ = n;
        else if (!strcmp(key, "max.grand")) maxGrand_ = n;
        else if (!strcmp(key, "days")) days_ = n;
        else if (!strcmp(key, "lastday")) lastDay_ = (int)n;
        else return false;
        return true;
    }

    void clearProgressForLoad() {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        states_.assign(defs().size(), State());
        keyCounts_.clear();
        for (uint64_t& c : cum_) c = 0;
        maxStars_ = starMax_ = maxGrand_ = days_ = 0;
        lastDay_ = 0;
        pollInit_ = false;
    }

    // For logs and tests.
    uint64_t metricValue(const Def& d, uint64_t playSeconds, bool firstGame) const {
        HostHeapScope hostHeap;
        std::lock_guard<std::mutex> lock(mutex_);
        return valueLocked(d, playSeconds, firstGame);
    }

private:
    uint64_t valueLocked(const Def& d, uint64_t playSeconds, bool firstGame) const {
        switch (d.metric) {
        case M_FIRST_GAME: return firstGame ? 1 : 0;
        case M_STARS: return maxStars_;
        case M_GRAND_STARS: return maxGrand_;
        case M_STAR_BITS: return cum_[0];
        case M_COINS: return cum_[1];
        case M_ONE_UPS: return cum_[2];
        case M_DEATHS: return cum_[3];
        case M_PLAY_SECONDS: return playSeconds;
        case M_GALAXIES: {
            uint64_t n = 0;
            for (const auto& kv : keyCounts_) {
                if (kv.first.compare(0, 2, "v:") == 0 && kv.second > 0 && isCountedGalaxy(kv.first.c_str() + 2)) n++;
            }
            return n;
        }
        case M_DAYS: return days_;
        case M_KEY: {
            auto it = keyCounts_.find(d.key);
            return it == keyCounts_.end() ? 0 : it->second;
        }
        case M_COMETS: return cum_[4];
        case M_FULL_STARS: return (starMax_ > 0 && maxStars_ >= starMax_) ? 1 : 0;
        case M_ALL: return 0;
        }
        return 0;
    }

    static bool met(const Def& d, uint64_t v, const State& s) {
        if (d.metric == M_FIRST_GAME) return v >= 1;
        if (d.cumulative) return v >= s.base + d.target;
        return v >= d.target && (s.base == 0 || v > s.base);
    }

    uint64_t unlockedCountLocked(bool excludeAll) const {
        uint64_t n = 0;
        const std::vector<Def>& d = defs();
        for (size_t i = 0; i < d.size(); i++) {
            if (excludeAll && d[i].metric == M_ALL) continue;
            if (states_[i].unlockedAt != 0) n++;
        }
        return n;
    }

    bool allOthersUnlockedLocked() const { return unlockedCountLocked(true) + 1 == defs().size(); }

    void resetLocked(size_t i, uint64_t playSeconds, bool firstGame) {
        const Def& d = defs()[i];
        State& s = states_[i];
        s.unlockedAt = 0;
        if (d.metric == M_ALL) {
            s.base = 1;  // unlocks again only after another achievement was reset and re-earned
        } else if (d.metric == M_FIRST_GAME) {
            s.base = 0;
        } else {
            s.base = valueLocked(d, playSeconds, firstGame);
        }
    }

    mutable std::mutex mutex_;
    std::vector<State> states_;
    std::map<std::string, uint64_t> keyCounts_;
    uint64_t cum_[5] = {0, 0, 0, 0, 0};  // bits, coins, oneups, deaths, comets
    uint64_t maxStars_ = 0, starMax_ = 0, maxGrand_ = 0, days_ = 0;
    int lastDay_ = 0;
    bool pollInit_ = false;
    int lastUserFile_ = -1;
    int lastBits_ = 0, lastCoins_ = 0, lastLives_ = 0;
};

}  // namespace achv

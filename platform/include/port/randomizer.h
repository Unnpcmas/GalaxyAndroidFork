// Seeded randomizer engine. Pure C++ (no game headers) so it can be tested on a PC.
//
// What it decides (all deterministic for a given seed + flags):
//   * galaxies:  which galaxy is behind each galaxy door of the Astro Dome
//                (a cycle over the regular galaxies, so no door leads to its own galaxy);
//   * stars:     which mission of a galaxy is loaded when a mission is picked
//                (a permutation of the normal missions; the star is still recorded
//                under the mission the player picked);
//   * enemies:   which enemy a placed enemy is replaced with (within a family of
//                enemies that behave alike);
//   * items:     which mushroom a placed mushroom is replaced with; power-ups are swapped only
//                when F_POWERUPS is set and challenge mode is off.
//
// Power-ups are important for progress (some missions need a specific one), so they are never
// swapped unless asked for separately; challenge mode forbids it. The other categories only
// permute things, so every galaxy, mission and star stays reachable exactly once.
#pragma once

#include <stdint.h>
#include <string.h>

#include <stdlib.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace rando {

// Every table in this file lives for the whole run, but on the Android port a plain `operator new`
// called from an emulated game thread allocates inside the game's JKRHeap (see decomp/src/JSystem/
// JKernel/JKRHeap.cpp).  Those heaps are torn down and reused on every scene change, so a std::map
// built there ends up with nodes that the game later overwrites: the next lookup or clear() then
// follows a corrupted tree (endless loop in find(), or endless recursion in clear(), as seen in
// the field logs).  All containers here therefore take their memory straight from libc.
template <typename T>
struct HostAlloc {
    using value_type = T;
    HostAlloc() = default;
    template <typename U>
    HostAlloc(const HostAlloc<U>&) {}
    T* allocate(size_t n) {
        void* p = ::malloc(n ? n * sizeof(T) : 1);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { ::free(p); }
    template <typename U>
    bool operator==(const HostAlloc<U>&) const { return true; }
    template <typename U>
    bool operator!=(const HostAlloc<U>&) const { return false; }
};

using HString = std::basic_string<char, std::char_traits<char>, HostAlloc<char>>;
template <typename T>
using HVec = std::vector<T, HostAlloc<T>>;
template <typename K, typename V>
using HMap = std::map<K, V, std::less<K>, HostAlloc<std::pair<const K, V>>>;

enum Flags : unsigned {
    F_GALAXIES = 1u << 0,
    F_STARS = 1u << 1,
    F_ENEMIES = 1u << 2,
    F_ITEMS = 1u << 3,
    F_CHALLENGE = 1u << 4,
    F_POWERUPS = 1u << 5,  // swap power-ups too (can leave a mission impossible; off in challenge mode)
};

// splitmix64: tiny, well mixed, identical on every platform.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0u; }
};

inline uint64_t hashText(const char* text, uint64_t basis = 1469598103934665603ull) {
    uint64_t h = basis;
    for (; text && *text; text++) {
        h ^= (uint8_t)*text;
        h *= 1099511628211ull;
    }
    return h;
}

// The seed shown to the player is a six digit number.
inline uint64_t seedFromNumber(unsigned number) { return hashText("galaxy-rando", 0x5EEDull + number); }

// Sattolo's algorithm: a random single cycle, so no element stays in place.
template <typename V>
inline void cycle(V& v, Rng& rng) {
    for (size_t i = v.size(); i > 1; i--) {
        std::swap(v[i - 1], v[rng.below((uint32_t)(i - 1))]);
    }
}

// ---- Pools ---------------------------------------------------------------------
// Names come from the game's own tables (GameEventFlagTable / NameObjFactory).
inline const HVec<const char*>& galaxyPool() {
    static const HVec<const char*> v = {
        "EggStarGalaxy",      "HoneyBeeKingdomGalaxy", "HeavenlyBeachGalaxy", "IceVolcanoGalaxy",
        "SandClockGalaxy",    "CosmosGardenGalaxy",    "StarDustGalaxy",      "BattleShipGalaxy",
        "PhantomGalaxy",      "FactoryGalaxy",         "HellProminenceGalaxy", "ReverseKingdomGalaxy",
        "OceanRingGalaxy",    "CannonFleetGalaxy",
    };
    return v;
}

inline bool inGalaxyPool(const char* stage) {
    if (!stage) return false;
    for (const char* g : galaxyPool()) {
        if (!strcmp(g, stage)) return true;
    }
    return false;
}

// Enemies that move and fight in a similar way.
inline const HVec<HVec<const char*>>& enemyFamilies() {
    static const HVec<HVec<const char*>> v = {
        {"Kuribo", "BombHei"},
        {"Teresa", "BombTeresa"},
    };
    return v;
}

// Items that are harmless to swap.
inline const HVec<HVec<const char*>>& itemFamilies() {
    static const HVec<HVec<const char*>> v = {
        {"KinokoOneUp", "KinokoLifeUp"},
    };
    return v;
}

// Power-ups: a mission can need a specific one, so challenge mode leaves them alone.
inline const HVec<HVec<const char*>>& powerUpFamilies() {
    static const HVec<HVec<const char*>> v = {
        {"MorphItemNeoHopper", "MorphItemNeoBee", "MorphItemNeoTeresa", "MorphItemNeoIce", "MorphItemNeoFire"},
    };
    return v;
}

// ---- Engine --------------------------------------------------------------------
class Engine {
public:
    // (Re)builds the tables when the seed or the flags changed. Cheap when they did not.
    void configure(uint64_t seed, unsigned flags) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (built_ && seed == seed_ && flags == flags_) return;
        seed_ = seed;
        flags_ = flags;
        built_ = true;
        galaxy_.clear();
        objects_.clear();
        stageScenarios_.clear();

        if (flags & F_GALAXIES) {
            HVec<const char*> targets = galaxyPool();
            Rng rng(hashText("galaxies", seed));
            cycle(targets, rng);
            const HVec<const char*>& pool = galaxyPool();
            for (size_t i = 0; i < pool.size(); i++) galaxy_[pool[i]] = targets[i];
        }
        if (flags & F_ENEMIES) addFamilies(enemyFamilies(), "enemies");
        if (flags & F_ITEMS) {
            addFamilies(itemFamilies(), "items");
        }
        if ((flags & F_POWERUPS) && !(flags & F_CHALLENGE)) addFamilies(powerUpFamilies(), "powerups");
    }

    bool active() const {
        return built_ && (flags_ & (F_GALAXIES | F_STARS | F_ENEMIES | F_ITEMS | F_POWERUPS)) != 0;
    }
    unsigned flags() const { return flags_; }
    uint64_t seed() const { return seed_; }

    // Every replacement the tables hold, one line each (for the log).
    std::vector<std::string> describe() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const auto& kv : galaxy_) out.push_back(std::string("galaxy door ") + kv.first.c_str() + " -> " + kv.second);
        for (const auto& kv : objects_) out.push_back(std::string("object ") + kv.first.c_str() + " -> " + kv.second);
        return out;
    }

    // Galaxy behind a galaxy door; the same name when it is not randomized.
    const char* galaxy(const char* stage) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = galaxy_.find(stage ? stage : "");
        return it == galaxy_.end() ? stage : it->second;
    }

    // Mission that is loaded when `selected` (1 based) is picked; `normalCount` is how many
    // plain missions (no comet, no hidden star) the galaxy has, in order from 1.
    int scenario(const char* stage, int selected, int normalCount) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!(flags_ & F_STARS) || !inGalaxyPool(stage) || normalCount < 2 || selected < 1 || selected > normalCount) {
            return selected;
        }
        HString key(stage);
        key += "#";
        key += std::to_string(normalCount).c_str();
        auto it = stageScenarios_.find(key);
        if (it == stageScenarios_.end()) {
            HVec<int> order;
            for (int i = 1; i <= normalCount; i++) order.push_back(i);
            Rng rng(hashText(stage, hashText("stars", seed_)));
            cycle(order, rng);
            it = stageScenarios_.emplace(key, order).first;
        }
        return it->second[(size_t)(selected - 1)];
    }

    // Object (enemy / item) that replaces a placed object; the same name when not randomized.
    const char* object(const char* name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (objects_.empty() || !name) return name;
        auto it = objects_.find(name);
        return it == objects_.end() ? name : it->second;
    }

private:
    void addFamilies(const HVec<HVec<const char*>>& families, const char* salt) {
        for (size_t f = 0; f < families.size(); f++) {
            HVec<const char*> targets = families[f];
            Rng rng(hashText(salt, seed_ + (uint64_t)f * 7919ull));
            cycle(targets, rng);
            for (size_t i = 0; i < families[f].size(); i++) objects_[families[f][i]] = targets[i];
        }
    }

    std::mutex mutex_;
    bool built_ = false;
    uint64_t seed_ = 0;
    unsigned flags_ = 0;
    HMap<HString, const char*> galaxy_;
    HMap<HString, const char*> objects_;
    HMap<HString, HVec<int>> stageScenarios_;
};

}  // namespace rando

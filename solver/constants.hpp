#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <random>

constexpr int NUM_WORDS = 500'000;
constexpr int MAX_CACHE_SIZE = 20'000;
constexpr int MAX_RACK_SIZE = 16;
constexpr int MAX_WORD_LEN = 16;
constexpr char WILDCARD_CHAR = '?';
constexpr double WILDCARD_PTS = 1.0;

enum class Gem : std::uint8_t
{
    NONE = 0,
    AMETHYST = 1,
    EMERALD = 2,
    GARNET = 3,
    SAPPHIRE = 4,
    RUBY = 5,
    CRYSTAL = 6,
    DIAMOND = 7
};

using Point = double;

struct RuntimeConfig
{
    bool gems_enabled = false;
    bool rainbow = false;
    bool prescramble = true;
    double power = 0.0;
    bool powered = false;
    int base_damage_bonus = 0;
    int enemy_armour = 0;
    bool treasure_equipped = false;
    int active_weakness_cat = -1;
    double active_weakness_boost = 1.0;
    std::array<Point, 26> letter_points = {
        1, 1.25, 1.25, 1, 1, 1.25, 1, 1.25, 1, 1.75, 1.75, 1, 1.25, 1, 1, 1.25, 2.75, 1, 1, 1, 1, 1.5, 1.5, 2, 1.5, 2
    };

    bool operator==(const RuntimeConfig&) const = default;
};

inline RuntimeConfig& config()
{
    static RuntimeConfig c;
    return c;
}

// Cached SearchResults depend on RuntimeConfig fields that are NOT part of
// the cache key (letter_points, armour, bonuses, rainbow). Mutating those
// must bump this epoch; BestWordCache self-clears lazily on mismatch. Bumped
// only between requests, so relaxed ordering suffices for worker reads.
inline std::atomic<std::uint64_t>& config_epoch()
{
    static std::atomic<std::uint64_t> e{0};
    return e;
}

struct SearchTables
{
    std::array<Point, 8> letter_value_tiers_desc{};
    int n_value_tiers = 0;
    int wildcard_tier = 0;
    std::array<std::int8_t, 26> letter_to_tier{};
};

inline SearchTables& search_tables()
{
    static SearchTables t;
    return t;
}

inline void rebuild_search_tables()
{
    const auto& lp = config().letter_points;

    std::array<Point, 26> tmp{};
    int n = 0;
    for (int i = 0; i < 26; ++i)
    {
        const Point v = lp[i];
        if (v <= 0.0)
            continue;
        bool seen = false;
        for (int j = 0; j < n; ++j)
        {
            if (tmp[j] == v)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
            tmp[n++] = v;
    }

    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            if (tmp[j] > tmp[i])
                std::swap(tmp[i], tmp[j]);
        }
    }

    // At most 7 value tiers + the trailing zero tier (used by 0-point letters
    // and broken tiles). More distinct letter values than that are merged
    // into the 7th tier, which over-estimates their value -- the search bound
    // stays sound.
    if (n > 7)
        n = 7;

    SearchTables& st = search_tables();
    st.letter_value_tiers_desc.fill(0.0);
    for (int i = 0; i < n; ++i)
        st.letter_value_tiers_desc[i] = tmp[i];
    st.n_value_tiers = n + 1;
    st.wildcard_tier = 0;
    for (int i = 0; i < n; ++i)
    {
        if (st.letter_value_tiers_desc[i] == WILDCARD_PTS)
        {
            st.wildcard_tier = i;
            break;
        }
    }

    for (int i = 0; i < 26; ++i)
    {
        const Point v = lp[i];
        st.letter_to_tier[i] = static_cast<std::int8_t>(v <= 0.0 ? n : n - 1);
        for (int j = 0; j < n; ++j)
        {
            if (st.letter_value_tiers_desc[j] == v)
            {
                st.letter_to_tier[i] = static_cast<std::int8_t>(j);
                break;
            }
        }
    }
}

inline constexpr std::array<int, 27> max_letter_frequency = {
    4, 3, 3, 4, 4, 3, 4, 3, 4, 2, 2, 4, 3, 4, 3, 4, 2, 4, 4, 4, 4, 3, 3, 2, 3, 2, 16
};

inline constexpr std::array<int, 15> quarterHeartsTable = {1, 2, 3, 4, 6, 8, 11, 14, 18, 22, 27, 32, 38, 44, 52};

inline constexpr std::array<double, 7> gem_power_tiers_desc = {1.0, 0.5, 0.35, 0.3, 0.25, 0.2, 0.15};

inline std::discrete_distribution<>& distribution()
{
    static thread_local std::discrete_distribution<> d{
        9420.0 / 97380, 2200.0 / 97380, 2000.0 / 97380, 4200.0 / 97380, 12700.0 / 97380,
        2000.0 / 97380, 2500.0 / 97380, 2000.0 / 97380, 7330.0 / 97380, 700.0 / 97380,
        700.0 / 97380, 4000.0 / 97380, 2300.0 / 97380, 6000.0 / 97380, 7330.0 / 97380,
        2300.0 / 97380, 600.0 / 97380, 6900.0 / 97380, 5600.0 / 97380, 6000.0 / 97380,
        4400.0 / 97380, 1600.0 / 97380, 1500.0 / 97380, 500.0 / 97380, 2100.0 / 97380,
        500.0 / 97380,
    };
    return d;
}
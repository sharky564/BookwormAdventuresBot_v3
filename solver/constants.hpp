#pragma once
#include <array>
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
};

inline RuntimeConfig& config()
{
    static RuntimeConfig c;
    return c;
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
        if (v <= 0.0)
        {
            st.letter_to_tier[i] = static_cast<std::int8_t>(n);
            continue;
        }
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
        0.0932, 0.0171, 0.0218, 0.0376, 0.13, 0.0235, 0.0257, 0.0252, 0.0723, 0.0077, 0.0056, 0.0466, 0.0214,
        0.0547, 0.0663, 0.0261, 0.0115, 0.0752, 0.0594, 0.0684, 0.0428, 0.0171, 0.0154, 0.006, 0.0252, 0.0042
    };
    return d;
}
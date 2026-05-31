#pragma once
#include "constants.hpp"
#include "tile.hpp"
#include "trie.hpp"
#include "utils.hpp"
#include "word.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <concepts>
#include <cstdint>
#include <queue>
#include <span>
#include <vector>

template <typename C>
concept Collector = requires(C c, int d, const TileList& p) {
    { c.threshold() } -> std::convertible_to<int>;
    c.offer(d, p);
};

struct ScoredWord
{
    int damage;
    TileList tiles;

    bool operator>(const ScoredWord& other) const noexcept
    {
        if (damage != other.damage) return damage > other.damage;
        if (tiles.size() != other.tiles.size())
            return tiles.size() > other.tiles.size();
        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            if (tiles[i].getLetter() != other.tiles[i].getLetter())
                return tiles[i].getLetter() > other.tiles[i].getLetter();
        }
        return false;
    }
};

using ScoredHeap = std::priority_queue<ScoredWord, std::vector<ScoredWord>, std::greater<>>;


struct RackState
{
    std::array<std::int8_t, 26> letter_counts{};
    std::array<std::int8_t, 26> non_gem_counts{};
    std::array<std::vector<Gem>, 26> gem_stacks{};
    std::int8_t wildcard_count = 0;

    std::array<std::int8_t, 8> value_bucket_counts{};
    std::array<std::int8_t, 7> gem_bucket_counts{};

    double power = 0.0;
    bool gems_enabled = false;
    double power_boost = 1.0;
    double treasure_boost = 1.5;
    bool treasure_equipped = false;
    int8_t weakness_cat = -1;
    double weakness_boost = 1.0;

    int base_damage_bonus = 0;
    int enemy_armour = 0;
};

struct BestCollector
{
    int best_damage = -1;
    TileList best_tiles;

    [[nodiscard]] int threshold() const noexcept { return best_damage; }

    void offer(int damage, const TileList& path)
    {
        if (damage > best_damage)
        {
            best_damage = damage;
            best_tiles = path;
        }
    }
};

struct TopNCollector
{
    ScoredHeap heap;
    int N;

    explicit TopNCollector(int n) : N(n) {}

    [[nodiscard]] int threshold() const noexcept
    {
        if (static_cast<int>(heap.size()) < N)
            return INT_MIN;
        return heap.top().damage - 1;
    }

    void offer(int damage, const TileList& path)
    {
        heap.push(ScoredWord{.damage = damage, .tiles = path});
        if (static_cast<int>(heap.size()) > N)
            heap.pop();
    }
};

struct ThresholdCollector
{
    std::vector<ScoredWord> kills;
    bool heap_mode = false;
    int threshold_damage;
    int max_candidates;
    int floor_minus_one;

    struct DamageGreater
    {
        bool operator()(const ScoredWord& a, const ScoredWord& b) const noexcept
        {
            return a.damage > b.damage;
        }
    };

    ThresholdCollector(int threshold_damage_, int max_candidates_)
        :
    threshold_damage(threshold_damage_),
    max_candidates(max_candidates_),
    floor_minus_one(threshold_damage_ - 1)
    {
        kills.reserve(max_candidates_ + 1);
    }

    [[nodiscard]] int threshold() const noexcept
    {
        if (!heap_mode)
            return floor_minus_one;
        return std::max(floor_minus_one, kills.front().damage - 1);
    }

    void offer(int damage, const TileList& path)
    {
        if (damage < threshold_damage)
            return;
        if (!heap_mode)
        {
            kills.push_back(ScoredWord{.damage = damage, .tiles = path});
            if (static_cast<int>(kills.size()) >= max_candidates)
            {
                std::make_heap(kills.begin(), kills.end(), DamageGreater{});
                heap_mode = true;
            }
            return;
        }
        if (damage > kills.front().damage)
        {
            std::pop_heap(kills.begin(), kills.end(), DamageGreater{});
            kills.back() = ScoredWord{.damage = damage, .tiles = path};
            std::push_heap(kills.begin(), kills.end(), DamageGreater{});
        }
    }
};

namespace detail {

inline int letter_to_value_tier(int letter_idx) noexcept { return search_tables().letter_to_tier[letter_idx]; }

inline int gem_to_tier(Gem g) noexcept
{
    switch (g)
    {
    case Gem::DIAMOND:
        return 0;
    case Gem::CRYSTAL:
        return 1;
    case Gem::RUBY:
        return 2;
    case Gem::GARNET:
        return 3;
    case Gem::SAPPHIRE:
        return 4;
    case Gem::EMERALD:
        return 5;
    case Gem::AMETHYST:
        return 6;
    default:
        return -1;
    }
}

template <typename CountT, std::size_t NC, typename ValT, std::size_t NV>
inline double top_k_sum(const std::array<CountT, NC>& counts, const std::array<ValT, NV>& tiers_desc, int k) noexcept
{
    constexpr std::size_t N = (NC < NV ? NC : NV);
    double sum = 0.0;
    for (std::size_t i = 0; i < N && k > 0; ++i)
    {
        const int take = std::min<int>(k, static_cast<int>(counts[i]));
        sum += take * static_cast<double>(tiers_desc[i]);
        k -= take;
    }
    return sum;
}

inline void push_real(RackState& st, int idx, Gem g) noexcept
{
    ++st.letter_counts[idx];
    ++st.value_bucket_counts[letter_to_value_tier(idx)];
    if (g == Gem::NONE)
    {
        ++st.non_gem_counts[idx];
    }
    else
    {
        auto& s = st.gem_stacks[idx];
        s.push_back(g);
        for (std::size_t i = s.size() - 1; i > 0; --i)
        {
            if (gemPower(s[i - 1]) > gemPower(s[i]))
                std::swap(s[i - 1], s[i]);
            else break;
        }
        const int t = gem_to_tier(g);
        if (t >= 0)
            ++st.gem_bucket_counts[t];
    }
}

inline void push_wildcard(RackState& st) noexcept
{
    ++st.wildcard_count;
    ++st.value_bucket_counts[search_tables().wildcard_tier];
}

inline void pop_wildcard(RackState& st) noexcept
{
    --st.wildcard_count;
    --st.value_bucket_counts[search_tables().wildcard_tier];
}

inline Gem pop_best_real(RackState& st, int idx) noexcept
{
    auto& stack = st.gem_stacks[idx];
    Gem chosen = Gem::NONE;
    if (!stack.empty())
    {
        chosen = stack.back();
        stack.pop_back();
        const int t = gem_to_tier(chosen);
        if (t >= 0)
            --st.gem_bucket_counts[t];
    }
    else
    {
        --st.non_gem_counts[idx];
    }
    --st.letter_counts[idx];
    --st.value_bucket_counts[letter_to_value_tier(idx)];
    return chosen;
}


template <int TrieSize, Collector C>
void dfs(
    const Trie<TrieSize>& trie,
    int node,
    double points_so_far,
    double gem_sum_so_far,
    TileList& path,
    RackState& st,
    C& collector
)
{
    const TrieNode& tn = trie.nodes[node];

    if (tn.finish_mask & WORD_FINISHED_BIT) [[unlikely]]
    {
        const double w_treasure =
            (st.treasure_equipped && (tn.finish_mask & bonus_bit_for(0 /*METAL*/))) ? st.treasure_boost : 1.0;
        const double w_weakness =
            (st.weakness_cat >= 0 && (tn.finish_mask & bonus_bit_for(st.weakness_cat))) ? st.weakness_boost : 1.0;
        const int dmg = compute_damage(
            points_so_far,
            gem_sum_so_far,
            st.power,
            st.gems_enabled,
            st.power_boost,
            w_treasure,
            w_weakness,
            st.base_damage_bonus,
            st.enemy_armour
        );
        collector.offer(dmg, path);
    }

    const int k = tn.max_depth;
    if (k <= 0) [[unlikely]]
        return;

    const double subtree_max = static_cast<double>(tn.max_subtree_points);
    const double top_k_extra = top_k_sum(st.value_bucket_counts, search_tables().letter_value_tiers_desc, k);
    const double max_extra_pts = std::min(top_k_extra, subtree_max);
    double max_extra_gem = 0.0;
    if (st.gems_enabled)
        max_extra_gem = top_k_sum(st.gem_bucket_counts, gem_power_tiers_desc, k);

    const double max_treasure =
        (st.treasure_equipped && (tn.subtree_bonuses & bonus_bit_for(0 /*METAL*/))) ? st.treasure_boost : 1.0;
    const double max_weakness =
        (st.weakness_cat >= 0 && (tn.subtree_bonuses & bonus_bit_for(st.weakness_cat))) ? st.weakness_boost : 1.0;
    const int ceiling = compute_damage(
        points_so_far + max_extra_pts,
        gem_sum_so_far + max_extra_gem,
        st.power,
        st.gems_enabled,
        st.power_boost,
        max_treasure,
        max_weakness,
        st.base_damage_bonus,
        st.enemy_armour
    );
    if (ceiling <= collector.threshold())
        return;

    const std::uint32_t count = std::popcount(tn.bitmap);
    const std::uint32_t base  = tn.children_offset;
    for (std::uint32_t slot = 0; slot < count; ++slot)
    {
        const int i = trie.child_letters[base + slot];
        const int child = trie.children[base + slot];

        if (st.letter_counts[i] > 0) [[likely]]
        {
            const Gem g = pop_best_real(st, i);
            path.emplace_back(static_cast<char>('A' + i), g);

            dfs(
                trie, child, points_so_far + config().letter_points[i],
                gem_sum_so_far + gemPower(g), path, st, collector
            );

            path.pop_back();
            push_real(st, i, g);
        }
        else if (st.wildcard_count > 0) [[unlikely]]
        {
            pop_wildcard(st);
            path.emplace_back(WILDCARD_CHAR, Gem::NONE);

            dfs(trie, child, points_so_far + WILDCARD_PTS, gem_sum_so_far, path, st, collector);

            path.pop_back();
            push_wildcard(st);
        }
    }
}

} // namespace detail

inline RackState build_rack_state(std::span<const Tile> tiles, double power, bool gems_enabled, double power_boost = 1.0)
{
    RackState st;
    st.power = power;
    st.gems_enabled = gems_enabled;
    st.power_boost = power_boost;
    st.base_damage_bonus = config().base_damage_bonus;
    st.enemy_armour = config().enemy_armour;
    st.treasure_equipped = config().treasure_equipped;
    st.treasure_boost = 1.5;
    st.weakness_cat = static_cast<int8_t>(config().active_weakness_cat);
    st.weakness_boost = config().active_weakness_boost;
    for (const Tile& t : tiles)
    {
        if (t.isWildcard())
            detail::push_wildcard(st);
        else
            detail::push_real(st, t.getLetter() - 'A', t.getGem());
    }
    return st;
}

struct SearchResult
{
    int damage = -1;
    TileList tiles;
};

template <int TrieSize>
SearchResult find_best_word(const Trie<TrieSize>& trie, RackState& state)
{
    BestCollector c;
    TileList path; path.reserve(MAX_WORD_LEN);
    detail::dfs(trie, Trie<TrieSize>::ROOT, 0.0, 0.0, path, state, c);
    return SearchResult{c.best_damage, std::move(c.best_tiles)};
}

template <int TrieSize>
ScoredHeap find_top_words(const Trie<TrieSize>& trie, RackState& state, int N)
{
    TopNCollector c(N);
    TileList path; path.reserve(MAX_WORD_LEN);
    detail::dfs(trie, Trie<TrieSize>::ROOT, 0.0, 0.0, path, state, c);
    return std::move(c.heap);
}

template <int TrieSize>
std::vector<ScoredWord> find_kill_words(
    const Trie<TrieSize>& trie,
    RackState& state,
    int threshold_damage,
    int max_candidates
)
{
    ThresholdCollector c(threshold_damage, max_candidates);
    TileList path; path.reserve(MAX_WORD_LEN);
    detail::dfs(trie, Trie<TrieSize>::ROOT, 0.0, 0.0, path, state, c);

    std::sort(c.kills.begin(), c.kills.end(),
              [](const ScoredWord& a, const ScoredWord& b) {
                  return a.damage > b.damage;
              });
    return std::move(c.kills);
}
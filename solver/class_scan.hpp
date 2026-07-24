#pragma once
//==============================================================================
// class_scan.hpp -- anagram-class scan search engine.
//
// Damage is a function of the tile MULTISET plus the resolved word's bonus
// mask, so instead of walking the trie per query, the dictionary is grouped
// into anagram classes (sorted-letter keys) and a query is one linear pass:
//   phase 1: SIMD subset prefilter over a uint32 letters-mask per class
//   phase 2: exact 26-count check (saturating subtract); damage per
//            bonus-mask variant; collector offers per member word
// One pass serves every collector and is immune to the wildcard DFS blowup.
// Classes are extracted FROM the finalized trie so dictionary/bonus data
// cannot diverge from the DFS engine; the trie remains authoritative for
// trace/is_word.
//
// Parity contract vs DFS: identical damage multisets; tie-breaking (which
// anagram/tile assignment is returned among equal damage) may differ.
//==============================================================================
#include "constants.hpp"
#include "rack_search.hpp"
#include "trie.hpp"
#include "word.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

class ClassScanIndex
{
public:
    [[nodiscard]] bool built() const noexcept { return mNumClasses > 0; }
    [[nodiscard]] int num_classes() const noexcept { return mNumClasses; }
    [[nodiscard]] int num_variants() const noexcept { return static_cast<int>(mVariantMask.size()); }

    // Extract all words (with their finish/bonus masks) from a finalized trie
    // and group them into anagram classes. Single-threaded; call at startup.
    template <int TrieSize>
    void build(const Trie<TrieSize>& trie)
    {
        struct Member { std::string word; std::uint8_t mask; };
        ankerl::unordered_dense::map<std::string, std::vector<Member>> groups;
        groups.reserve(200'000);

        std::string path;
        path.reserve(MAX_WORD_LEN);
        walk(trie, Trie<TrieSize>::ROOT, path, groups);

        // Deterministic class order, LONGEST first: BestCollector keeps the
        // first max it sees, so ties resolve to the longest word, which
        // cycles the rack faster in greedy rollouts (shortest-first measured
        // worse in simulate).
        std::vector<const std::string*> keys;
        keys.reserve(groups.size());
        for (const auto& [k, v] : groups)
            keys.push_back(&k);
        std::sort(keys.begin(), keys.end(), [](const std::string* a, const std::string* b) {
            if (a->size() != b->size())
                return a->size() > b->size();
            return *a < *b;
        });

        mNumClasses = static_cast<int>(keys.size());
        mMasks.resize(keys.size());
        mCounts.resize(keys.size());
        mVariantBegin.assign(keys.size() + 1, 0);
        mVariantMask.clear();
        mVariantWordBegin.clear();
        mWordOffsets.clear();
        mChars.clear();

        for (std::size_t c = 0; c < keys.size(); ++c)
        {
            const std::string& key = *keys[c];
            auto& members = groups[key];

            std::array<std::uint8_t, 32> counts{};
            std::uint32_t mask = 0;
            for (char ch : key)
            {
                const int li = ch - 'A';
                ++counts[li];
                mask |= (std::uint32_t{1} << li);
            }
            mMasks[c] = mask;
            mCounts[c] = counts;

            // Group members by bonus mask (most classes have one variant);
            // sort for deterministic member order.
            std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
                if (a.mask != b.mask)
                    return a.mask < b.mask;
                return a.word < b.word;
            });
            mVariantBegin[c] = static_cast<std::uint32_t>(mVariantMask.size());
            std::size_t i = 0;
            while (i < members.size())
            {
                const std::uint8_t vm = members[i].mask;
                mVariantMask.push_back(vm);
                mVariantWordBegin.push_back(static_cast<std::uint32_t>(mWordOffsets.size()));
                while (i < members.size() && members[i].mask == vm)
                {
                    mWordOffsets.push_back(static_cast<std::uint32_t>(mChars.size()));
                    mChars.insert(mChars.end(), members[i].word.begin(), members[i].word.end());
                    mChars.push_back('\0');
                    ++i;
                }
            }
        }
        mVariantBegin[keys.size()] = static_cast<std::uint32_t>(mVariantMask.size());
        mVariantWordBegin.push_back(static_cast<std::uint32_t>(mWordOffsets.size()));

        refresh_points();
    }

    // Recompute per-class points from config().letter_points. Must be called
    // from a single-threaded context (startup, or the serve dispatch thread
    // after a config op) -- never while search workers are running.
    void refresh_points()
    {
        const auto& lp = config().letter_points;
        mLetterPoints = lp;
        mPoints.resize(mNumClasses);
        for (int c = 0; c < mNumClasses; ++c)
        {
            const auto& counts = mCounts[c];
            double p = 0.0;
            for (int i = 0; i < 26; ++i)
                p += static_cast<double>(counts[i]) * lp[i];
            mPoints[c] = p;
        }
    }

    [[nodiscard]] bool points_current() const noexcept { return mLetterPoints == config().letter_points; }

    template <Collector C>
    void search(RackState& st, C& collector) const;

private:
    template <int TrieSize, typename Groups>
    void walk(const Trie<TrieSize>& trie, int node, std::string& path, Groups& groups)
    {
        const TrieNode& tn = trie.nodes[node];
        if (tn.finish_mask & WORD_FINISHED_BIT)
        {
            std::string key = path;
            std::sort(key.begin(), key.end());
            groups[std::move(key)].push_back({path, static_cast<std::uint8_t>(tn.finish_mask & ALL_BONUS_BITS)});
        }
        const std::uint32_t count = std::popcount(tn.bitmap);
        const std::uint32_t base = tn.children_offset;
        for (std::uint32_t slot = 0; slot < count; ++slot)
        {
            path.push_back(static_cast<char>('A' + trie.child_letters[base + slot]));
            walk(trie, trie.children[base + slot], path, groups);
            path.pop_back();
        }
    }

    int mNumClasses = 0;
    std::vector<std::uint32_t> mMasks;                    // phase-1 letters-present mask
    std::vector<std::array<std::uint8_t, 32>> mCounts;    // phase-2 letter counts (26 used)
    std::vector<double> mPoints;                          // class points under mLetterPoints
    std::array<Point, 26> mLetterPoints{};                // letter_points mPoints was built with
    std::vector<std::uint32_t> mVariantBegin;             // class -> [begin, end) into variant arrays
    std::vector<std::uint8_t> mVariantMask;               // bonus mask per variant
    std::vector<std::uint32_t> mVariantWordBegin;         // variant -> [begin, end) into mWordOffsets
    std::vector<std::uint32_t> mWordOffsets;              // word -> offset into mChars (NUL-terminated)
    std::vector<char> mChars;
};

inline ClassScanIndex& class_index()
{
    static ClassScanIndex idx;
    return idx;
}

template <Collector C>
void ClassScanIndex::search(RackState& st, C& collector) const
{
    // Rack-side facts. have[] includes broken tiles (they spell normally);
    // normal_have[] excludes them for the points math.
    std::array<std::uint8_t, 32> have{};
    std::array<std::uint8_t, 26> normal_have{};
    std::uint32_t rack_mask = 0;
    std::uint32_t broken_mask = 0;
    for (int i = 0; i < 26; ++i)
    {
        have[i] = static_cast<std::uint8_t>(st.letter_counts[i]);
        normal_have[i] = static_cast<std::uint8_t>(st.letter_counts[i] - st.broken_counts[i]);
        if (st.letter_counts[i] > 0)
            rack_mask |= (std::uint32_t{1} << i);
        if (st.broken_counts[i] > 0)
            broken_mask |= (std::uint32_t{1} << i);
    }
    const int wildcards = st.wildcard_count;

    // gem_prefix[i][k] = power of the k best gems of letter i -- the same
    // strongest-gem-first greedy the DFS applies per consumed tile.
    std::array<std::array<double, MAX_RACK_SIZE + 1>, 26> gem_prefix;
    std::uint32_t gem_mask = 0;
    if (st.gems_enabled)
    {
        for (int i = 0; i < 26; ++i)
        {
            const GemStack& gs = st.gem_stacks[i];
            if (gs.n == 0)
                continue;
            gem_mask |= (std::uint32_t{1} << i);
            auto& pref = gem_prefix[i];
            pref[0] = 0.0;
            for (int k = 0; k < gs.n; ++k)
                pref[k + 1] = pref[k] + gemPower(gs.g[gs.n - 1 - k]);
            for (int k = gs.n; k < MAX_RACK_SIZE; ++k)
                pref[k + 1] = pref[gs.n];
        }
    }

    const int n = mNumClasses;
    const std::uint32_t* masks = mMasks.data();

    // Phase 1: survivors of the letters-mask test.
    static thread_local std::vector<std::int32_t> survivors;
    survivors.clear();
    survivors.reserve(static_cast<std::size_t>(n) / 4 + 64);

#if defined(__AVX2__)
    {
        const __m256i vrack = _mm256_set1_epi32(static_cast<int>(~rack_mask));
        const __m256i vzero = _mm256_setzero_si256();
        int c = 0;
        if (wildcards == 0)
        {
            for (; c + 8 <= n; c += 8)
            {
                const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(masks + c));
                const __m256i missing = _mm256_and_si256(m, vrack);
                const __m256i ok = _mm256_cmpeq_epi32(missing, vzero);
                std::uint32_t bits = static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(ok)));
                while (bits)
                {
                    const int lane = std::countr_zero(bits);
                    bits &= bits - 1;
                    survivors.push_back(c + lane);
                }
            }
        }
        else
        {
            const __m256i lut = _mm256_setr_epi8(
                0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
                0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
            const __m256i low_mask = _mm256_set1_epi8(0x0F);
            const __m256i ones16 = _mm256_set1_epi16(1);
            const __m256i vwc = _mm256_set1_epi32(wildcards);
            for (; c + 8 <= n; c += 8)
            {
                const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(masks + c));
                const __m256i missing = _mm256_and_si256(m, vrack);
                const __m256i lo = _mm256_and_si256(missing, low_mask);
                const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(missing, 4), low_mask);
                const __m256i cnt8 = _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo), _mm256_shuffle_epi8(lut, hi));
                // per-byte popcounts -> per-lane totals
                const __m256i cnt16 = _mm256_maddubs_epi16(cnt8, _mm256_set1_epi8(1));
                const __m256i cnt32 = _mm256_madd_epi16(cnt16, ones16);
                const __m256i ok = _mm256_cmpgt_epi32(cnt32, vwc);  // reject if popcount > wildcards
                std::uint32_t bits = static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(ok)));
                bits = ~bits & 0xFFu;
                while (bits)
                {
                    const int lane = std::countr_zero(bits);
                    bits &= bits - 1;
                    survivors.push_back(c + lane);
                }
            }
        }
        for (; c < n; ++c)
        {
            const std::uint32_t missing = masks[c] & ~rack_mask;
            if (static_cast<int>(std::popcount(missing)) <= wildcards)
                survivors.push_back(c);
        }
    }
#else
    for (int c = 0; c < n; ++c)
    {
        const std::uint32_t missing = masks[c] & ~rack_mask;
        if (static_cast<int>(std::popcount(missing)) <= wildcards)
            survivors.push_back(c);
    }
#endif

    // Phase 2: exact count check + damage per bonus-mask variant.
    const auto& lp = *st.letter_points;
#if defined(__AVX2__)
    const __m256i vhave = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(have.data()));
    const __m256i vzero2 = _mm256_setzero_si256();
#endif

    for (const std::int32_t c : survivors)
    {
        const auto& need = mCounts[c];
        int deficit_total;
#if defined(__AVX2__)
        const __m256i vneed = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(need.data()));
        const __m256i vdef = _mm256_subs_epu8(vneed, vhave);
        const __m256i sad = _mm256_sad_epu8(vdef, vzero2);
        deficit_total = _mm256_extract_epi32(sad, 0) + _mm256_extract_epi32(sad, 2)
                      + _mm256_extract_epi32(sad, 4) + _mm256_extract_epi32(sad, 6);
#else
        deficit_total = 0;
        for (int i = 0; i < 26; ++i)
            deficit_total += std::max(0, static_cast<int>(need[i]) - static_cast<int>(have[i]));
#endif
        if (deficit_total > wildcards)
            continue;

        // Points: class points minus deficit letters' points plus wildcard
        // points per covered deficit (rare path).
        double points = mPoints[c];
        if (deficit_total > 0)
        {
            for (int i = 0; i < 26; ++i)
            {
                const int d = static_cast<int>(need[i]) - static_cast<int>(have[i]);
                if (d > 0)
                    points += d * (WILDCARD_PTS - lp[i]);
            }
        }
        // Broken tiles consumed (normal tiles are used first) score 0.
        if (std::uint32_t bm = mMasks[c] & broken_mask; bm != 0)
        {
            while (bm)
            {
                const int i = std::countr_zero(bm);
                bm &= bm - 1;
                const int used = std::min<int>(need[i], have[i]);
                const int broken_used = used - std::min<int>(used, normal_have[i]);
                if (broken_used > 0)
                    points -= broken_used * lp[i];
            }
        }

        // Gem power of the tiles the greedy assignment would consume.
        double gem_sum = 0.0;
        if (std::uint32_t gm = (st.gems_enabled ? (mMasks[c] & gem_mask) : 0u); gm != 0)
        {
            while (gm)
            {
                const int i = std::countr_zero(gm);
                gm &= gm - 1;
                const int used_real = std::min<int>(need[i], have[i]);
                gem_sum += gem_prefix[i][used_real];
            }
        }

        // Damage per bonus-mask variant; offer every member word so the
        // damage multiset matches the DFS engine exactly.
        const std::uint32_t vb = mVariantBegin[c];
        const std::uint32_t ve = mVariantBegin[c + 1];
        for (std::uint32_t v = vb; v < ve; ++v)
        {
            const std::uint8_t vm = mVariantMask[v];
            const double w_treasure =
                (st.treasure_equipped && (vm & bonus_bit_for(0))) ? st.treasure_boost : 1.0;
            const double w_weakness =
                (st.weakness_cat >= 0 && (vm & bonus_bit_for(st.weakness_cat))) ? st.weakness_boost : 1.0;
            const int dmg = compute_damage(
                points, gem_sum, st.power, st.gems_enabled, st.power_boost,
                w_treasure, w_weakness, st.base_damage_bonus, st.enemy_armour);

            if (dmg <= collector.threshold())
                continue;

            const std::uint32_t wb = mVariantWordBegin[v];
            const std::uint32_t we = mVariantWordBegin[v + 1];
            for (std::uint32_t w = wb; w < we; ++w)
            {
                if (dmg <= collector.threshold())
                    break;  // heap cutoff may have risen while offering
                const char* word = mChars.data() + mWordOffsets[w];

                // Build the tile assignment the DFS would produce: gem tiles
                // first (strongest gem first), then plain, then broken;
                // wildcards cover the last occurrences of deficit letters.
                TileList tiles;
                std::array<std::uint8_t, 26> occ{};
                for (const char* p = word; *p; ++p)
                {
                    const int i = *p - 'A';
                    const int k = occ[i]++;
                    if (k < static_cast<int>(normal_have[i]))
                    {
                        const GemStack& gs = st.gem_stacks[i];
                        const Gem g = (k < gs.n) ? gs.g[gs.n - 1 - k] : Gem::NONE;
                        tiles.emplace_back(*p, g);
                    }
                    else if (k < static_cast<int>(have[i]))
                    {
                        tiles.emplace_back(*p, Gem::NONE, true);
                    }
                    else
                    {
                        tiles.emplace_back(WILDCARD_CHAR, Gem::NONE);
                    }
                }
                collector.offer(dmg, tiles);
            }
        }
    }
}

// Engine-dispatching counterparts of find_best_word / find_top_words /
// find_kill_words. The runtime switch lets serve requests, bench runs and the
// gen modes A/B the two engines on identical workloads.
enum class SearchEngine : std::uint8_t { DFS = 0, Scan = 1 };

inline SearchEngine& search_engine()
{
    static SearchEngine e = SearchEngine::DFS;
    return e;
}

inline bool use_scan_engine()
{
    return search_engine() == SearchEngine::Scan && class_index().built();
}

inline int rack_tile_total(const RackState& st) noexcept
{
    int total = st.wildcard_count;
    for (int i = 0; i < 26; ++i)
        total += st.letter_counts[i];
    return total;
}

// Hybrid dispatch (bench-calibrated): the scan wins on large racks, the DFS
// bound prunes small ones to microseconds. Wildcards inflate the phase-1
// survivor set, which single-best queries never amortize (enumeration does).
inline constexpr int SCAN_MIN_TILES = 12;

inline bool scan_for_best(const RackState& st) noexcept
{
    return use_scan_engine() && st.wildcard_count == 0 && rack_tile_total(st) >= SCAN_MIN_TILES;
}

inline bool scan_for_enum(const RackState& st) noexcept
{
    return use_scan_engine() && rack_tile_total(st) >= SCAN_MIN_TILES;
}

// Upper bound on any word's damage from this rack (points clamp at 16, all
// rack gems counted) -- lets unreachable kill thresholds skip the scan.
inline int rack_damage_ceiling(const RackState& st) noexcept
{
    double gem_total = 0.0;
    if (st.gems_enabled)
    {
        for (int i = 0; i < 26; ++i)
        {
            const GemStack& gs = st.gem_stacks[i];
            for (int k = 0; k < gs.n; ++k)
                gem_total += gemPower(gs.g[k]);
        }
    }
    const double max_treasure = st.treasure_equipped ? st.treasure_boost : 1.0;
    const double max_weakness = (st.weakness_cat >= 0) ? st.weakness_boost : 1.0;
    return compute_damage(16.0, gem_total, st.power, st.gems_enabled, st.power_boost,
                          max_treasure, max_weakness, st.base_damage_bonus, st.enemy_armour);
}

template <int TrieSize>
SearchResult engine_find_best_word(const Trie<TrieSize>& trie, RackState& state)
{
    if (scan_for_best(state))
    {
        BestCollector c;
        class_index().search(state, c);
        return SearchResult{c.best_damage, c.best_tiles};
    }
    return find_best_word(trie, state);
}

template <int TrieSize>
ScoredHeap engine_find_top_words(const Trie<TrieSize>& trie, RackState& state, int N)
{
    if (scan_for_enum(state))
    {
        TopNCollector c(N);
        class_index().search(state, c);
        return std::move(c.heap);
    }
    return find_top_words(trie, state, N);
}

template <int TrieSize>
std::vector<ScoredWord> engine_find_kill_words(
    const Trie<TrieSize>& trie, RackState& state, int threshold_damage, int max_candidates)
{
    if (scan_for_enum(state))
    {
        if (rack_damage_ceiling(state) < threshold_damage)
            return {};
        ThresholdCollector c(threshold_damage, max_candidates);
        class_index().search(state, c);
        std::sort(c.kills.begin(), c.kills.end(),
                  [](const ScoredWord& a, const ScoredWord& b) {
                      return a.damage > b.damage;
                  });
        return std::move(c.kills);
    }
    return find_kill_words(trie, state, threshold_damage, max_candidates);
}

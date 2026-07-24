#pragma once
#include "class_scan.hpp"
#include "constants.hpp"
#include "rack.hpp"
#include "rack_search.hpp"
#include "trie.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <ankerl/unordered_dense.h>


struct RackKey
{
    std::array<std::uint8_t, 26> letter_counts;
    std::array<std::uint8_t, 26> gem_counts;
    std::uint8_t wildcards;
    std::uint8_t flags;
    std::uint8_t weakness_cat;
    std::uint8_t _pad;
    std::uint64_t power_bits;

    bool operator==(const RackKey& other) const noexcept
    {
        return std::memcmp(this, &other, sizeof(RackKey)) == 0;
    }
};
static_assert(sizeof(RackKey) == 64, "RackKey should be 64 bytes (one cache line)");
static_assert(std::is_trivially_copyable_v<RackKey>);

inline RackKey makeRackKey(const TileList& tiles, double power, bool powered) noexcept
{
    RackKey k{};
    const RuntimeConfig& cfg = config();
    for (const Tile& t : tiles)
    {
        if (t.isWildcard())
        {
            ++k.wildcards;
            continue;
        }
        const int li = t.getLetter() - 'A';
        ++k.letter_counts[li];
        if (cfg.gems_enabled)
            k.gem_counts[li] += static_cast<std::uint8_t>(t.getGem()) + 1;
    }
    std::uint8_t flags = 0;
    if (powered)
        flags |= 1u;
    if (cfg.treasure_equipped)
        flags |= 2u;
    if (cfg.gems_enabled)
        flags |= 4u;
    if (cfg.active_weakness_cat >= 0)
    {
        flags |= 8u;
        k.weakness_cat = static_cast<std::uint8_t>(cfg.active_weakness_cat);
    }
    k.flags = flags;
    std::memcpy(&k.power_bits, &power, sizeof(double));
    return k;
}

struct RackKeyHash
{
    std::size_t operator()(const RackKey& k) const noexcept
    {
        std::uint64_t buf[8];
        std::memcpy(buf, &k, sizeof(RackKey));
        std::uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < 8; ++i)
        {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(h);
    }
};

class BestWordCache
{
public:
    explicit BestWordCache(std::size_t capacity = MAX_CACHE_SIZE)
        : mHalfCap(std::max<std::size_t>(1, capacity / 2))
    {
        mCurr.reserve(mHalfCap + 1);
        mPrev.reserve(mHalfCap + 1);
    }

    SearchResult lookup(const TileList& tiles, const Trie<NUM_WORDS>& trie, double power = 0.0, bool powered = false)
    {
        if (const std::uint64_t epoch = config_epoch().load(std::memory_order_relaxed); epoch != mEpoch)
        {
            mCurr.clear();
            mPrev.clear();
            mEpoch = epoch;
        }
        const RackKey key = makeRackKey(tiles, power, powered);
        if (const auto it = mCurr.find(key); it != mCurr.end())
        {
            ++mHits;
            return it->second;
        }
        if (const auto it = mPrev.find(key); it != mPrev.end())
        {
            // Promote so entries that stay hot survive generation rotations.
            ++mHits;
            const SearchResult sr = it->second;
            mPrev.erase(it);
            insert(key, sr);
            return sr;
        }
        ++mMisses;

        RackState state = build_rack_state(tiles, power, config().gems_enabled, powered ? 1.25 : 1.0);
        SearchResult sr = engine_find_best_word(trie, state);
        if (!sr.tiles.empty())
        {
            sr.refill_gem = expected_gem_for(sr.tiles);
            sr.refill_wildcard = refill_wildcard_for(sr.tiles);
        }
        insert(key, sr);
        return sr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return mCurr.size() + mPrev.size(); }
    [[nodiscard]] std::uint64_t hits() const noexcept { return mHits; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return mMisses; }

private:
    using Map = ankerl::unordered_dense::map<RackKey, SearchResult, RackKeyHash>;

    // Generational eviction: O(1) amortized and approximately LRU. When the
    // active generation fills, it replaces the previous one, which is dropped
    // wholesale (its buffer is reused via swap+clear).
    void insert(const RackKey& key, const SearchResult& sr)
    {
        if (mCurr.size() >= mHalfCap)
        {
            std::swap(mCurr, mPrev);
            mCurr.clear();
        }
        mCurr.emplace(key, sr);
    }

    Map mCurr;
    Map mPrev;
    std::size_t mHalfCap;
    std::uint64_t mEpoch = config_epoch().load(std::memory_order_relaxed);
    std::uint64_t mHits = 0;
    std::uint64_t mMisses = 0;
};

template <typename Rng>
inline double rolloutValue(
    Rack rack,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    Rng& rng,
    BestWordCache& cache,
    double power = 0.0,
    bool powered = false
)
{
    double total = 0.0;
    for (int t = 0; t < horizon; ++t)
    {
        const SearchResult sr = cache.lookup(rack.getTiles(), trie, power, powered);
        if (sr.damage <= 0 || sr.tiles.empty())
            break;
        total += sr.damage;

        rack.playTiles(sr.tiles);
        rack.regenerateTiles(sr.refill_gem, sr.refill_wildcard, rng);
    }
    return total;
}

template <typename Rng>
inline double rolloutValueInto(
    const Rack& source,
    Rack& scratch,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    Rng& rng,
    BestWordCache& cache,
    double power = 0.0,
    bool powered = false
)
{
    scratch = source;
    double total = 0.0;
    for (int t = 0; t < horizon; ++t)
    {
        const SearchResult sr = cache.lookup(scratch.getTiles(), trie, power, powered);
        if (sr.damage <= 0 || sr.tiles.empty())
            break;
        total += sr.damage;

        scratch.playTiles(sr.tiles);
        scratch.regenerateTiles(sr.refill_gem, sr.refill_wildcard, rng);
    }
    return total;
}

// --- Overkill gem drop -----------------------------------------------------
//
// When a kill overshoots the enemy's remaining HP, a bonus gem is dropped on
// a random tile in the NEXT rack. The gem tier is a function of the excess
// damage (damage - threshold). Note this ordering is the game's own balancing
// table and is NOT the same as the damage-formula gem tiers (e.g. garnet
// comes before sapphire here).
//
//   excess  8-12  -> amethyst
//   excess 13-20  -> emerald
//   excess 21-32  -> garnet
//   excess 33-44  -> sapphire
//   excess 45-56  -> ruby
//   excess 57-80  -> crystal
//   excess  81+   -> diamond
//   excess  < 8   -> none (no gem)
inline Gem overkill_gem_for_excess(int excess) noexcept
{
    if (excess < 8)   return Gem::NONE;
    if (excess <= 12) return Gem::AMETHYST;
    if (excess <= 20) return Gem::EMERALD;
    if (excess <= 32) return Gem::GARNET;
    if (excess <= 44) return Gem::SAPPHIRE;
    if (excess <= 56) return Gem::RUBY;
    if (excess <= 80) return Gem::CRYSTAL;
    return Gem::DIAMOND;
}

// Kill-aware leaf metric.
//
// Instead of summing greedy damage over `horizon` turns (which rewards
// raw accumulation), this scores how reliably/soon the rack can land a
// single word that meets `next_threshold` -- i.e. kill the enemy that
// will be faced next turn. Each turn t (0-indexed) contributes
//
//     gamma^t * kill_indicator(best_damage_t, next_threshold)
//
// where kill_indicator is a margin-smoothed step: 1.0 once damage clears
// the threshold, ramping up over a small margin band below it so that
// "almost lethal" racks are valued above hopeless ones. The earliest
// killing turn dominates via the gamma discount; once a turn kills, we
// stop (the enemy is dead). If `drop_gem` is set, it is placed on a random
// tile of the residual before the rollout begins (overkill gem credit).
template <typename Rng>
inline double rolloutKillValueInto(
    const Rack& source,
    Rack& scratch,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    int next_threshold,
    Gem drop_gem,
    Rng& rng,
    BestWordCache& cache,
    double power = 0.0,
    bool powered = false,
    double gamma = 0.7,
    double margin = 8.0
)
{
    scratch = source;
    if (drop_gem != Gem::NONE)
        scratch.dropGemOnRandomTile(drop_gem, rng);

    const double GAMMA = gamma;          // earlier kills weighted higher
    const double MARGIN = margin;        // smoothing band below threshold
    const double thr = static_cast<double>(next_threshold);

    double value = 0.0;
    double discount = 1.0;
    for (int t = 0; t < horizon; ++t)
    {
        const SearchResult sr = cache.lookup(scratch.getTiles(), trie, power, powered);
        if (sr.tiles.empty())
            break;
        const double dmg = static_cast<double>(sr.damage);

        // Margin-smoothed kill indicator in [0, 1].
        double ind;
        if (dmg >= thr)
            ind = 1.0;
        else if (dmg >= thr - MARGIN)
            ind = (dmg - (thr - MARGIN)) / MARGIN;  // linear ramp over the band
        else
            ind = 0.0;

        value += discount * ind;
        if (ind >= 1.0)
            break;  // enemy dead this turn; no need to look further

        scratch.playTiles(sr.tiles);
        scratch.regenerateTiles(sr.refill_gem, sr.refill_wildcard, rng);
        discount *= GAMMA;
    }
    return value;
}


template <typename Rng>
inline double monteCarloRackValue(
    const Rack& rack,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    int min_sims,
    int max_sims,
    double se_target,
    Rng& rng,
    BestWordCache& cache,
    int* out_n_sims = nullptr,
    double power = 0.0,
    bool powered = false,
    int next_threshold = 0,
    Gem drop_gem = Gem::NONE,
    double gamma = 0.7,
    double margin = 8.0,
    bool force_plain = false
)
{
    // Kill-aware leaf metric is gated on a positive next_threshold. When it
    // is 0 (the default), fall back to the original sum-of-damage rollout so
    // existing callers and A/B comparisons are unaffected. `force_plain`
    // overrides this to always use the plain sum-of-damage rollout even when
    // next_threshold > 0 (used by the A/B harness to compare future metrics).
    const bool kill_aware = (next_threshold > 0) && !force_plain;

    if (horizon <= 1 && !kill_aware)
    {
        const auto sr = cache.lookup(rack.getTiles(), trie, power, powered);
        if (out_n_sims)
            *out_n_sims = 1;
        return static_cast<double>(sr.damage);
    }

    double mean = 0.0;
    double M2 = 0.0;
    int n = 0;
    Rack scratch(rack.size());
    while (n < max_sims)
    {
        const double v = kill_aware
            ? rolloutKillValueInto(rack, scratch, trie, horizon, next_threshold,
                                   drop_gem, rng, cache, power, powered, gamma, margin)
            : rolloutValueInto(rack, scratch, trie, horizon, rng, cache, power, powered);
        ++n;
        const double delta = v - mean;
        mean += delta / n;
        const double delta2 = v - mean;
        M2 += delta * delta2;

        if (n >= min_sims && n >= 2)
        {
            const double var = M2 / (n - 1);
            const double se = std::sqrt(var / n);
            if (se < se_target)
                break;
        }
    }
    if (out_n_sims)
        *out_n_sims = n;
    return mean;
}

struct GenerateConfig
{
    int num_racks = 100'000;
    int horizon = 5;
    int min_sims = 30;
    int max_sims = 300;
    double se_target = 0.5;
    int rack_size = MAX_RACK_SIZE;
    int num_threads = 0;
    std::uint64_t master_seed = 0xCAFEBABEull;
    int progress_every = 1000;
};

inline void generateDataParallel(
    const Trie<NUM_WORDS>& trie,
    const std::string& output_path,
    const GenerateConfig& cfg = {}
)
{
    const int n_threads = cfg.num_threads > 0 ? cfg.num_threads : std::max(1u, std::thread::hardware_concurrency() - 1);

    std::ofstream out(output_path, std::ios::app);
    if (!out.is_open())
    {
        std::cerr << std::format("Cannot open {}\n", output_path);
        return;
    }

    std::atomic<int> next_idx{0};
    std::atomic<long long> total_done{0};
    std::atomic<long long> total_sims{0};
    std::atomic<long long> total_hits{0};
    std::atomic<long long> total_miss{0};
    std::mutex out_mutex;
    const auto t0 = std::chrono::steady_clock::now();

    auto worker = [&](int thread_id)
    {
        FastRng rng(cfg.master_seed ^ (static_cast<std::uint64_t>(thread_id) * 0x9E3779B97F4A7C15ull));
        BestWordCache cache(MAX_CACHE_SIZE);
        std::string buffer;
        buffer.reserve(64 * 1024);

        while (true)
        {
            const int idx = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= cfg.num_racks)
                break;

            Rack rack(cfg.rack_size);
            rack.regenerateTiles(Gem::NONE, false, rng);

            int n_sims = 0;
            const double value = monteCarloRackValue(
                rack, trie, cfg.horizon, cfg.min_sims, cfg.max_sims, cfg.se_target, rng, cache, &n_sims
            );

            total_sims.fetch_add(n_sims, std::memory_order_relaxed);

            std::string rack_str;
            rack_str.reserve(rack.getTiles().size());
            for (const auto& t : rack.getTiles())
                rack_str += t.getLetter();
            buffer += std::format("{} {:.4f}\n", rack_str, value);

            if (buffer.size() > 32 * 1024)
            {
                std::lock_guard lk(out_mutex);
                out << buffer;
                buffer.clear();
            }

            const long long done = total_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cfg.progress_every > 0 && done % cfg.progress_every == 0)
            {
                const auto t1 = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(t1 - t0).count();
                std::cout << std::format(
                    "Generated {} / {}  ({:.1f}s, {:.0f}/s, {:.1f} sims/rack)\n",
                    done, cfg.num_racks, elapsed, done / elapsed, static_cast<double>(total_sims.load()) / done
                );
            }
        }

        if (!buffer.empty())
        {
            std::lock_guard lk(out_mutex);
            out << buffer;
        }
        total_hits.fetch_add(cache.hits(), std::memory_order_relaxed);
        total_miss.fetch_add(cache.misses(), std::memory_order_relaxed);
    };

    std::vector<std::jthread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    threads.clear();

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const long long hits = total_hits.load();
    const long long miss = total_miss.load();
    const double hit_pct = (hits + miss > 0) ? 100.0 * static_cast<double>(hits) / (hits + miss) : 0.0;
    std::cout << std::format(
        "Done. {} racks in {:.1f}s ({:.0f}/s). Cache: {} hits / {} misses ({:.1f}%)\n",
        cfg.num_racks, elapsed, cfg.num_racks / elapsed, hits, miss, hit_pct
    );
}

struct PerWordConfig
{
    int num_racks = 100'000;
    int num_top_words = 20;
    int horizon = 1;
    int num_simulations = 200;
    int rollout_min_sims = 8;
    int rollout_max_sims = 32;
    double rollout_se_target = 0.5;
    int crn_stride = 64;
    int rack_size = MAX_RACK_SIZE;
    int num_threads = 0;
    std::uint64_t master_seed = 0xC0FFEEBEEFull;
    int progress_every = 200;
    double power = 0.0;
    bool powered = false;
};

template <typename Rng>
inline double qValueOneStep(
    const Rack& rack,
    const Word& played_word,
    int num_simulations,
    int horizon,
    int inner_min_sims,
    int inner_max_sims,
    double inner_se_target,
    Rng& rng,
    BestWordCache& cache,
    const Trie<NUM_WORDS>& trie,
    const int* crn_draws = nullptr,
    int crn_stride = 64,
    double power = 0.0,
    bool powered = false
)
{
    Rack residual = rack;
    residual.playWord(played_word);

    const Gem refill_gem = played_word.expectedGem();
    const bool refill_wildcard = played_word.checkWildcard();

    double total_future = 0.0;
    for (int s = 0; s < num_simulations; ++s)
    {
        Rack draw(residual.getTiles(), residual.size());
        if (crn_draws)
            draw.regenerateTilesCRN(refill_gem, refill_wildcard, crn_draws + static_cast<std::size_t>(s) * crn_stride);
        else
            draw.regenerateTiles(refill_gem, refill_wildcard, rng);

        if (horizon <= 1)
        {
            const auto sr = cache.lookup(draw.getTiles(), trie, power, powered);
            total_future += (sr.damage > 0 ? sr.damage : 0);
        }
        else
        {
            int n_sims_used = 0;
            const double v = monteCarloRackValue(
                draw, trie, horizon, inner_min_sims, inner_max_sims,
                inner_se_target, rng, cache, &n_sims_used, power, powered
            );
            total_future += v;
        }
    }
    return total_future / num_simulations;
}

inline void generatePerWordDataParallel(
    const Trie<NUM_WORDS>& trie,
    const std::string& output_path,
    const PerWordConfig& cfg = {}
)
{
    const int n_threads = cfg.num_threads > 0 ? cfg.num_threads : std::max(1u, std::thread::hardware_concurrency());

    std::ofstream out(output_path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << std::format("Cannot open {}\n", output_path);
        return;
    }
    out << "rack\tword\tnow\tfuture\ttotal\n";

    std::atomic<int> next_idx{0};
    std::atomic<long long> total_done{0};
    std::atomic<long long> total_rows{0};
    std::atomic<long long> total_hits{0};
    std::atomic<long long> total_miss{0};
    std::mutex out_mutex;
    const auto t0 = std::chrono::steady_clock::now();

    std::cout << std::format(
        "Per-word generation: {} racks, {} threads, {} candidates/rack, {} sims/candidate, horizon={}\n",
        cfg.num_racks, n_threads, cfg.num_top_words, cfg.num_simulations, cfg.horizon
    );

    auto worker = [&](int thread_id)
    {
        FastRng rng(cfg.master_seed ^ (static_cast<std::uint64_t>(thread_id) * 0x9E3779B97F4A7C15ull));
        BestWordCache cache(MAX_CACHE_SIZE);
        std::vector<int> crn_buffer;
        crn_buffer.reserve(static_cast<std::size_t>(cfg.num_simulations) * cfg.crn_stride);
        std::string buffer;
        buffer.reserve(64 * 1024);

        while (true)
        {
            const int idx = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= cfg.num_racks)
                break;

            Rack rack(cfg.rack_size);
            rack.regenerateTiles(Gem::NONE, false, rng);

            std::string rack_str;
            rack_str.reserve(rack.getTiles().size());
            for (const auto& t : rack.getTiles())
                rack_str += t.getLetter();

            ScoredHeap heap = rack.generateWordlist(trie, cfg.num_top_words, cfg.power, cfg.powered);
            if (heap.empty())
            {
                total_done.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::vector<ScoredWord> candidates;
            candidates.reserve(heap.size());
            while (!heap.empty())
            {
                candidates.push_back(heap.top());
                heap.pop();
            }

            const std::size_t N = static_cast<std::size_t>(cfg.num_simulations);
            const std::size_t S = static_cast<std::size_t>(cfg.crn_stride);
            crn_buffer.resize(N * S);
            const auto& sampler = letter_sampler();
            for (int& v : crn_buffer)
                v = sampler(rng);

            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
            {
                std::string wstr;
                wstr.reserve(it->tiles.size());
                for (const auto& t : it->tiles)
                    wstr += t.getLetter();

                const Word w(it->tiles);
                const double future = qValueOneStep(
                    rack, w, cfg.num_simulations, cfg.horizon, cfg.rollout_min_sims, cfg.rollout_max_sims,
                    cfg.rollout_se_target, rng, cache, trie, crn_buffer.data(), cfg.crn_stride, cfg.power, cfg.powered
                );
                const double total = static_cast<double>(it->damage) + future;

                buffer += std::format("{}\t{}\t{}\t{:.4f}\t{:.4f}\n", rack_str, wstr, it->damage, future, total);
                total_rows.fetch_add(1, std::memory_order_relaxed);
            }

            if (buffer.size() > 32 * 1024)
            {
                std::lock_guard lk(out_mutex);
                out << buffer;
                buffer.clear();
            }

            const long long done = total_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (cfg.progress_every > 0 && done % cfg.progress_every == 0)
            {
                const auto t1 = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(t1 - t0).count();
                std::cout << std::format(
                    "  {}/{}  ({:.1f}s, {:.0f} racks/s, {} rows so far)\n",
                    done, cfg.num_racks, elapsed, done / elapsed, total_rows.load()
                );
            }
        }

        if (!buffer.empty())
        {
            std::lock_guard lk(out_mutex);
            out << buffer;
        }
        total_hits.fetch_add(cache.hits(), std::memory_order_relaxed);
        total_miss.fetch_add(cache.misses(), std::memory_order_relaxed);
    };

    std::vector<std::jthread> threads;
    threads.reserve(n_threads);
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    threads.clear();

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(t1 - t0).count();
    const long long hits = total_hits.load();
    const long long miss = total_miss.load();
    const double hit_pct = (hits + miss > 0) ? 100.0 * static_cast<double>(hits) / (hits + miss) : 0.0;
    std::cout << std::format(
        "Done. {} racks ({} rows) in {:.1f}s ({:.0f} racks/s). Cache hit rate: {:.1f}% ({} hits / {} misses)\n",
        cfg.num_racks, total_rows.load(), elapsed, cfg.num_racks / elapsed, hit_pct, hits, miss
    );
}
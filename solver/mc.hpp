#pragma once
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
#include <fstream>
#include <iostream>
#include <list>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <ankerl/unordered_dense.h>


inline std::string makeRackKey(const TileList& tiles, double power = 0.0, bool powered = false)
{
    std::array<std::pair<char, Gem>, MAX_RACK_SIZE> pairs;
    const std::size_t n = tiles.size();
    for (std::size_t i = 0; i < n; ++i)
        pairs[i] = {tiles[i].getLetter(), tiles[i].getGem()};

    std::sort(pairs.begin(), pairs.begin() + n);

    std::string key;
    key.reserve(n * 2 + 16);
    for (std::size_t i = 0; i < n; ++i)
    {
        key += pairs[i].first;
        if (config().gems_enabled)
            key += static_cast<char>(static_cast<unsigned>(pairs[i].second) + '0');
    }
    if (power != 0.0 || powered)
    {
        key += '_';
        key += std::to_string(power);
        key += '_';
        key += (powered ? "1" : "0");
    }
    if (config().treasure_equipped)
        key += "_T";
    if (config().active_weakness_cat >= 0)
    {
        key += "_W";
        key += static_cast<char>('0' + config().active_weakness_cat);
    }
    return key;
}

class BestWordCache
{
public:
    explicit BestWordCache(std::size_t capacity = MAX_CACHE_SIZE)
        : mCap(capacity)
    {
        mMap.reserve(capacity);
    }

    SearchResult lookup(const TileList& tiles, const Trie<NUM_WORDS>& trie, double power = 0.0, bool powered = false)
    {
        const std::string key = makeRackKey(tiles, power, powered);
        if (const auto it = mMap.find(key); it != mMap.end())
        {
            mList.splice(mList.begin(), mList, it->second);
            ++mHits;
            return it->second->second;
        }
        ++mMisses;

        RackState state = build_rack_state(tiles, power, config().gems_enabled, powered ? 1.25 : 1.0);
        SearchResult sr = find_best_word(trie, state);

        mList.emplace_front(key, sr);
        mMap[mList.front().first] = mList.begin();
        if (mMap.size() > mCap)
        {
            mMap.erase(mList.back().first);
            mList.pop_back();
        }
        return sr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return mMap.size(); }
    [[nodiscard]] std::uint64_t hits() const noexcept { return mHits; }
    [[nodiscard]] std::uint64_t misses() const noexcept { return mMisses; }

private:
    using Item = std::pair<std::string, SearchResult>;
    using ItemList = std::list<Item>;
    using Map = ankerl::unordered_dense::map<std::string, typename ItemList::iterator>;

    ItemList mList;
    Map mMap;
    std::size_t mCap;
    std::uint64_t mHits = 0;
    std::uint64_t mMisses = 0;
};

inline double rolloutValue(
    Rack rack,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    std::mt19937& rng,
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

        Word w(sr.tiles);
        rack.playWord(w, rng);
    }
    return total;
}


inline double monteCarloRackValue(
    const Rack& rack,
    const Trie<NUM_WORDS>& trie,
    int horizon,
    int min_sims,
    int max_sims,
    double se_target,
    std::mt19937& rng,
    BestWordCache& cache,
    int* out_n_sims = nullptr,
    double power = 0.0,
    bool powered = false
)
{
    if (horizon <= 1)
    {
        const auto sr = cache.lookup(rack.getTiles(), trie, power, powered);
        if (out_n_sims)
            *out_n_sims = 1;
        return static_cast<double>(sr.damage);
    }

    double mean = 0.0;
    double M2 = 0.0;
    int n = 0;
    while (n < max_sims)
    {
        const double v = rolloutValue(rack, trie, horizon, rng, cache, power, powered);
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
    const int n_threads = cfg.num_threads > 0 ? cfg.num_threads : std::max(1u, std::thread::hardware_concurrency());

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
        std::mt19937 rng(cfg.master_seed ^ (static_cast<std::uint64_t>(thread_id) * 0x9E3779B97F4A7C15ull));
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

inline double qValueOneStep(
    const Rack& rack,
    const Word& played_word,
    int num_simulations,
    int horizon,
    int inner_min_sims,
    int inner_max_sims,
    double inner_se_target,
    std::mt19937& rng,
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
        std::mt19937 rng(cfg.master_seed ^ (static_cast<std::uint64_t>(thread_id) * 0x9E3779B97F4A7C15ull));
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
            auto& dist = distribution();
            for (int& v : crn_buffer)
                v = dist(rng);

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
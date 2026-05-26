//==============================================================================
// bench.cpp -- benchmark + correctness fingerprinting for rack_search.
//
// Records two things per (mode, rack, param) tuple:
//   1. A 64-bit fingerprint of the result, used for regression checks.
//   2. min/median/mean wall time over N iterations (1 warmup, untimed).
//
// Usage:
//   ./bench <wordlist>                          print table + fingerprints
//   ./bench <wordlist> --save FILE also save fingerprints
//   ./bench <wordlist> --check FILE compare against saved
//   ./bench <wordlist> --iter N iterations per test (default 5)
//   ./bench <wordlist> --csv emit CSV instead of a table
//
// Recommended workflow:
//   make bench
//   ./bench ba1-dictionary.txt --save bench_baseline.txt
//   <apply optimisations, rebuild>
//   ./bench ba1-dictionary.txt --check bench_baseline.txt
//
// Each row carries two fingerprints:
//   strong - damage + tile letters + gems (sensitive to tie-breaking)
//   weak - sorted damage multiset only (set of damages is preserved)
//
// If `strong` mismatches but `weak` matches, the optimisation merely picked a
// different word among ties (acceptable). If `weak` mismatches, the result
// set itself changed -- a real regression.
//
// No third-party deps; just std::chrono.
//==============================================================================

#include "rack.hpp"
#include "trie.hpp"
#include "constants.hpp"
#include "utils.hpp"
#include "tile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <iostream>
#include <numeric>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;


std::string tile_seq_string(std::span<const Tile> tiles)
{
    std::string s;
    s.reserve(tiles.size() * 2);
    for (const auto& t : tiles)
    {
        s += t.getLetter();
        s += gemCharacter(t.getGem());
    }
    return s;
}

void canonicalize(std::vector<ScoredWord>& v)
{
    std::sort(v.begin(), v.end(),
        [](const ScoredWord& a, const ScoredWord& b)
        {
            if (a.damage != b.damage)
                return a.damage > b.damage;
            return tile_seq_string(a.tiles) < tile_seq_string(b.tiles);
        });
}

std::vector<ScoredWord> drain(ScoredHeap heap)
{
    std::vector<ScoredWord> v;
    v.reserve(heap.size());
    while (!heap.empty())
    {
        v.push_back(heap.top());
        heap.pop();
    }
    return v;
}

inline std::uint64_t mix8(std::uint64_t h, std::uint8_t b) noexcept
{
    h ^= b;
    h *= 1099511628211ULL;
    return h;
}

inline std::uint64_t mix32(std::uint64_t h, std::uint32_t v) noexcept
{
    h = mix8(h, std::uint8_t(v & 0xFF));
    h = mix8(h, std::uint8_t((v >> 8) & 0xFF));
    h = mix8(h, std::uint8_t((v >> 16)& 0xFF));
    h = mix8(h, std::uint8_t((v >> 24)& 0xFF));
    return h;
}

std::uint64_t fp_strong(const std::vector<ScoredWord>& v)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& w : v)
    {
        h = mix32(h, std::uint32_t(w.damage));
        for (const auto& t : w.tiles)
        {
            h = mix8(h, std::uint8_t(t.getLetter()));
            h = mix8(h, std::uint8_t(t.getGem()));
        }
        h = mix8(h, 0xFF);
    }
    return h;
}

std::uint64_t fp_weak(const std::vector<ScoredWord>& v)
{
    std::vector<int> d; d.reserve(v.size());
    for (const auto& w : v)
        d.push_back(w.damage);
    std::sort(d.begin(), d.end(), std::greater<int>());
    std::uint64_t h = 1469598103934665603ULL;
    for (int x : d)
        h = mix32(h, std::uint32_t(x));
    return h;
}

std::uint64_t fp_strong_best(const SearchResult& r)
{
    std::uint64_t h = 1469598103934665603ULL;
    h = mix32(h, std::uint32_t(r.damage));
    for (const auto& t : r.tiles)
    {
        h = mix8(h, std::uint8_t(t.getLetter()));
        h = mix8(h, std::uint8_t(t.getGem()));
    }
    return h;
}
std::uint64_t fp_weak_best(const SearchResult& r)
{
    std::uint64_t h = 1469598103934665603ULL;
    h = mix32(h, std::uint32_t(r.damage));
    return h;
}

struct TestRack
{
    std::string name;
    std::string letters;
    std::vector<Gem> gems;
    double power = 0.0;
    bool powered = false;
    bool gems_enabled = false;
};

std::vector<TestRack> default_racks()
{
    using G = Gem;
    return {
        {"r1_short_7", "ABCDEFG", {}, 0.0, false, false},
        {"r2_mid_8", "ARSTEOIN", {}, 0.0, false, false},
        {"r3_full_16", "ARSTEOINLDCMPUBO", {}, 0.0, false, false},
        {"r4_vowels_15", "AEIOUAEIOUAEIOU", {}, 0.0, false, false},
        {"r5_qz_13", "QUIZJOKERBOXY", {}, 0.0, false, false},
        {"r6_wild2_10", "ARSTE?N?LI", {}, 0.0, false, false},
        {"r7_powered_15", "ARSTEOINLDCMPUB", {}, 50.0, true,  false},
        {"r8_gems_8", "ARSTEOIN", {G::DIAMOND, G::NONE, G::RUBY, G::NONE, G::SAPPHIRE, G::NONE, G::NONE, G::EMERALD}, 0.0, false, true },
    };
}


struct Stats
{
    double min_ms = 0;
    double med_ms = 0;
    double mean_ms = 0;
    int n = 0;
};

Stats compute_stats(std::vector<double> ms)
{
    std::sort(ms.begin(), ms.end());
    Stats s;
    s.n = static_cast<int>(ms.size());
    s.min_ms = ms.front();
    s.med_ms = ms[ms.size() / 2];
    s.mean_ms = std::accumulate(ms.begin(), ms.end(), 0.0) / ms.size();
    return s;
}

template <typename F>
Stats time_it(int iter, F&& fn)
{
    fn();
    std::vector<double> samples; samples.reserve(iter);
    for (int i = 0; i < iter; ++i)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return compute_stats(std::move(samples));
}

struct FpPair
{
    std::uint64_t strong = 0;
    std::uint64_t weak = 0;
};

std::string make_key(std::string_view mode, std::string_view rack, int param)
{
    std::string s;
    s += mode;
    s += ':';
    s += rack;
    s += ':';
    s += std::to_string(param);
    return s;
}

std::unordered_map<std::string, FpPair> load_baseline(const std::string& path)
{
    std::unordered_map<std::string, FpPair> m;
    std::ifstream f(path);
    std::string key;
    while (f >> key)
    {
        FpPair p;
        f >> std::hex >> p.strong >> p.weak >> std::dec;
        if (!f)
            break;
        m.emplace(std::move(key), p);
    }
    return m;
}

void save_baseline(const std::string& path, const std::vector<std::pair<std::string, FpPair>>& entries)
{
    std::ofstream f(path, std::ios::trunc);
    for (const auto& [k, p] : entries)
        f << k << '\t' << std::hex << p.strong << '\t' << p.weak << std::dec << '\n';
}


struct Row
{
    std::string mode, rack;
    int param = 0;
    Stats stats;
    FpPair fp;
};

void print_table(const std::vector<Row>& rows)
{
    std::printf(
        "%-10s %-16s %7s %10s %10s %10s %6s   %-16s %-16s\n",
        "mode", "rack", "param", "min_ms", "med_ms", "mean_ms", "iters", "fp_strong", "fp_weak"
    );
    std::printf("%s\n", std::string(112, '-').c_str());
    for (const auto& r : rows)
    {
        std::printf(
            "%-10s %-16s %7d %10.3f %10.3f %10.3f %6d   %016llx %016llx\n",
            r.mode.c_str(), r.rack.c_str(), r.param, r.stats.min_ms, r.stats.med_ms, r.stats.mean_ms, r.stats.n,
            static_cast<unsigned long long>(r.fp.strong), static_cast<unsigned long long>(r.fp.weak)
        );
    }
}

void print_csv(const std::vector<Row>& rows)
{
    std::printf("mode,rack,param,min_ms,med_ms,mean_ms,iters,fp_strong,fp_weak\n");
    for (const auto& r : rows)
    {
        std::printf(
            "%s,%s,%d,%.6f,%.6f,%.6f,%d,%llx,%llx\n",
            r.mode.c_str(), r.rack.c_str(), r.param, r.stats.min_ms, r.stats.med_ms, r.stats.mean_ms, r.stats.n,
            static_cast<unsigned long long>(r.fp.strong), static_cast<unsigned long long>(r.fp.weak)
        );
    }
}

} // namespace


int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <wordlist> [--save FILE] [--check FILE] [--iter N] [--csv]\n", argv[0]);
        return 2;
    }

    const std::string wordlist = argv[1];
    std::string save_path, check_path;
    int iterations = 5;
    bool csv = false;

    for (int i = 2; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--save"  && i + 1 < argc)
            save_path = argv[++i];
        else if (a == "--check" && i + 1 < argc)
            check_path = argv[++i];
        else if (a == "--iter"  && i + 1 < argc)
            iterations = std::atoi(argv[++i]);
        else if (a == "--csv")
            csv = true;
        else
        {
            std::fprintf(stderr, "unknown arg: %.*s\n", int(a.size()), a.data());
            return 2;
        }
    }

    config().gems_enabled = false;
    config().rainbow = false;
    config().prescramble = true;
    config().base_damage_bonus = 0;
    config().enemy_armour = 0;
    config().treasure_equipped = false;
    config().active_weakness_cat = -1;
    config().active_weakness_boost = 1.0;
    rebuild_search_tables();

    Trie<NUM_WORDS> trie;
    const auto loaded = trie.load(wordlist);
    if (!loaded)
    {
        std::fprintf(stderr, "Failed to load wordlist: %s\n", wordlist.c_str());
        return 1;
    }
    trie.finalize();
    std::fprintf(stderr,
        "Loaded %d words; %d nodes, %d edges. %d timed iter / test (+1 warmup).\n",
        *loaded, trie.node_count(), trie.edge_count(), iterations
    );

    const auto racks = default_racks();
    std::vector<Row> rows;
    std::vector<std::pair<std::string, FpPair>> baseline_out;
    baseline_out.reserve(racks.size() * 8);

    for (const auto& tr : racks)
    {
        const bool prev_gems = config().gems_enabled;
        config().gems_enabled = tr.gems_enabled;
        rebuild_search_tables();          // safe; cheap.

        const Rack rack(tr.letters, tr.gems);

        auto record = [&](std::string mode, int param, Stats s, FpPair fp)
        {
            rows.push_back({std::move(mode), tr.name, param, s, fp});
            baseline_out.emplace_back(make_key(rows.back().mode, tr.name, param), fp);
        };

        {
            FpPair fp;
            const auto s = time_it(iterations, [&]
            {
                const auto r = rack.bestWord(trie, tr.power, tr.powered);
                fp.strong = fp_strong_best(r);
                fp.weak = fp_weak_best(r);
                g_sink ^= fp.strong;
            });
            record("best_word", 0, s, fp);
        }

        for (int N : {20, 100})
        {
            FpPair fp;
            const auto s = time_it(iterations, [&]
            {
                auto heap = rack.generateWordlist(trie, N, tr.power, tr.powered);
                auto v = drain(std::move(heap));
                canonicalize(v);
                fp.strong = fp_strong(v);
                fp.weak = fp_weak(v);
                g_sink ^= fp.strong;
            });
            record("top_N", N, s, fp);
        }

        for (int threshold : {5, 15, 30, 60, 120})
        {
            FpPair fp;
            const auto s = time_it(iterations, [&]
            {
                auto v = rack.generateKills(trie, threshold, 500, tr.power, tr.powered);
                canonicalize(v);
                fp.strong = fp_strong(v);
                fp.weak = fp_weak(v);
                g_sink ^= fp.strong;
            });
            record("kills", threshold, s, fp);
        }

        config().gems_enabled = prev_gems;
    }

    if (csv)
        print_csv(rows);
    else
        print_table(rows);

    if (!save_path.empty())
    {
        save_baseline(save_path, baseline_out);
        std::fprintf(stderr, "Saved %zu fingerprints -> %s\n", baseline_out.size(), save_path.c_str());
    }

    int strong_diffs = 0, weak_diffs = 0, missing = 0;
    if (!check_path.empty())
    {
        const auto base = load_baseline(check_path);
        for (const auto& [k, fp] : baseline_out)
        {
            const auto it = base.find(k);
            if (it == base.end())
            {
                std::fprintf(stderr, "MISSING  %s\n", k.c_str());
                ++missing;
                continue;
            }
            const bool s_eq = (it->second.strong == fp.strong);
            const bool w_eq = (it->second.weak == fp.weak);
            if (!s_eq && !w_eq)
            {
                std::fprintf(stderr,
                    "REGRESSION %s: strong %llx->%llx  weak %llx->%llx\n",
                    k.c_str(),
                    (unsigned long long)it->second.strong, (unsigned long long)fp.strong,
                    (unsigned long long)it->second.weak,   (unsigned long long)fp.weak
                );
                ++strong_diffs; ++weak_diffs;
            }
            else if (!s_eq)
            {
                std::fprintf(stderr,
                    "tie-only %s: strong %llx -> %llx (weak unchanged)\n",
                    k.c_str(), (unsigned long long)it->second.strong, (unsigned long long)fp.strong
                );
                ++strong_diffs;
            }
        }
        std::fprintf(stderr,
            "Baseline check: %d missing, %d strong diffs, %d weak (=real) regressions.\n",
            missing, strong_diffs, weak_diffs
        );
    }

    // touch the sink
    if (g_sink == 0xDEADBEEFCAFEBABEULL)
        std::fputs("sink hit\n", stderr);

    return (weak_diffs == 0 && missing == 0) ? 0 : 1;
}
#pragma once
#include "constants.hpp"
#include "json_mini.hpp"
#include "mc.hpp"
#include "rack.hpp"
#include "rack_search.hpp"
#include "trie.hpp"
#include "utils.hpp"
#include "word.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <thread>
#include <unordered_set>
#include <string>
#include <string_view>
#include <vector>

namespace serve {

inline std::vector<Gem> parse_gems_string(std::string_view s, std::size_t len)
{
    std::vector<Gem> out(len, Gem::NONE);
    for (std::size_t i = 0; i < len && i < s.size(); ++i)
        out[i] = translateGem(s[i]);
    return out;
}

inline std::string_view gem_name(Gem g)
{
    switch (g)
    {
    case Gem::NONE:
        return "none";
    case Gem::AMETHYST:
        return "amethyst";
    case Gem::EMERALD:
        return "emerald";
    case Gem::SAPPHIRE:
        return "sapphire";
    case Gem::GARNET:
        return "garnet";
    case Gem::RUBY:
        return "ruby";
    case Gem::CRYSTAL:
        return "crystal";
    case Gem::DIAMOND:
        return "diamond";
    }
    return "none";
}

inline void write_tiles(jmini::Writer& w, const TileList& tiles)
{
    w.key("tiles").begin_arr();
    for (const auto& t : tiles)
    {
        w.begin_obj();
        const char c = t.getLetter();
        w.key("letter").str(std::string_view(&c, 1));
        w.key("gem").str(gem_name(t.getGem()));
        w.end_obj();
    }
    w.end_arr();
}

inline std::string handle_config(const jmini::Value& msg)
{
    RuntimeConfig& c = config();
    if (auto* v = msg.find("gems_enabled"); v && v->is_bool())
        c.gems_enabled = v->boolean();
    if (auto* v = msg.find("rainbow"); v && v->is_bool())
        c.rainbow = v->boolean();
    if (auto* v = msg.find("prescramble"); v && v->is_bool())
        c.prescramble = v->boolean();
    if (auto* v = msg.find("power"); v && v->is_number())
        c.power = v->num();
    if (auto* v = msg.find("powered"); v && v->is_bool())
        c.powered = v->boolean();
    if (auto* v = msg.find("base_damage_bonus"); v && v->is_number())
        c.base_damage_bonus = static_cast<int>(v->num());
    if (auto* v = msg.find("enemy_armour"); v && v->is_number())
        c.enemy_armour = static_cast<int>(v->num());
    if (auto* v = msg.find("treasure_equipped"); v && v->is_bool())
        c.treasure_equipped = v->boolean();
    if (auto* v = msg.find("weakness_cat"); v && v->is_number())
        c.active_weakness_cat = static_cast<int>(v->num());
    if (auto* v = msg.find("weakness_boost"); v && v->is_number())
        c.active_weakness_boost = v->num();
    if (auto* v = msg.find("letter_points"); v && v->is_array())
    {
        const auto& a = v->arr();
        if (a.size() != 26)
        {
            return jmini::Writer().begin_obj()
                .key("op").str("config")
                .key("error").str("letter_points must have 26 entries")
                .end_obj().take();
        }
        for (std::size_t i = 0; i < 26; ++i)
        {
            if (a[i].is_number())
                c.letter_points[i] = a[i].num();
        }
    }
    rebuild_search_tables();
    return jmini::Writer().begin_obj()
        .key("op").str("config")
        .key("ok").boolean(true)
        .end_obj().take();
}

template <int TrieSize>
inline std::string handle_trace(const Trie<TrieSize>& trie, const jmini::Value& msg)
{
    auto* word_v = msg.find("word");
    if (!word_v || !word_v->is_string())
    {
        return jmini::Writer().begin_obj()
            .key("op").str("trace")
            .key("error").str("word required")
            .end_obj().take();
    }

    const std::string& letters = word_v->str();
    std::string gems_str;
    if (auto* g = msg.find("gems"); g && g->is_string())
        gems_str = g->str();
    bool powered_override = false;
    bool powered_set = false;
    if (auto* p = msg.find("powered"); p && p->is_bool())
    {
        powered_override = p->boolean();
        powered_set = true;
    }

    TileList tiles;
    tiles.reserve(letters.size());
    auto parsed_gems = parse_gems_string(gems_str, letters.size());
    for (std::size_t i = 0; i < letters.size(); ++i)
        tiles.emplace_back(letters[i], parsed_gems[i]);

    Word w(tiles);
    const RuntimeConfig& c = config();

    std::uint8_t finish_mask = 0;
    bool word_in_dict = false;
    {
        int curr = Trie<TrieSize>::ROOT;
        bool ok = true;
        for (const auto& t : tiles)
        {
            if (t.isWildcard()) { ok = false; break; }
            const int letter_idx = t.getLetter() - 'A';
            const int next = trie.child_for_letter(curr, letter_idx);
            if (next < 0) { ok = false; break; }
            curr = next;
        }
        if (ok)
        {
            finish_mask = trie.nodes[curr].finish_mask;
            word_in_dict = (finish_mask & WORD_FINISHED_BIT) != 0;
        }
    }

    const double power = c.power;
    const bool gems_enabled = c.gems_enabled;
    const bool use_powered = powered_set ? powered_override : c.powered;
    const double power_boost = use_powered ? 1.25 : 1.0;
    const int base_damage_bonus = c.base_damage_bonus;
    const int armour = c.enemy_armour;

    const bool word_is_metal = (finish_mask & bonus_bit_for(0)) != 0;
    const bool treasure_applies = c.treasure_equipped && word_is_metal;
    const double treasure_boost = treasure_applies ? 1.5 : 1.0;

    const int weakness_cat = c.active_weakness_cat;
    const bool word_in_weakness = (weakness_cat >= 0) && (finish_mask & bonus_bit_for(weakness_cat));
    const double weakness_boost = word_in_weakness ? c.active_weakness_boost : 1.0;

    const double raw_points = w.getPoints();
    const int clamped = std::clamp(Utils::round_half_up(raw_points), 2, 16);
    const double Q = static_cast<double>(quarterHeartsTable[clamped - 2]);

    const double power_factor = 1.0 + power * 0.01;
    const double q_after_power = Q * power_factor;
    double gem_sum = 0.0;
    if (gems_enabled)
    {
        for (const auto& t : tiles)
            gem_sum += gemPower(t.getGem());
    }
    const double gem_contribution = gems_enabled ? std::ceil(gem_sum * Q) : 0.0;
    const double inner = q_after_power + gem_contribution;

    const double after_treasure = treasure_boost * inner;
    const double after_power_boost = power_boost * after_treasure;
    const double after_base = after_power_boost + static_cast<double>(base_damage_bonus);
    const double after_weakness = weakness_boost * after_base;
    const int floored = static_cast<int>(std::floor(after_weakness));
    const int after_armour = floored - armour;
    const int final_damage = std::max(0, after_armour);

    const int via_function = compute_damage(
        raw_points, gem_sum, power, gems_enabled,
        power_boost, treasure_boost, weakness_boost,
        base_damage_bonus, armour
    );

    static constexpr const char* CAT_NAMES[NUM_BONUS_CATS] = {"metal", "feline", "colors", "bones", "fruitsveg"};

    jmini::Writer w_out;
    w_out.begin_obj();
    w_out.key("op").str("trace");
    w_out.key("word").str(letters);

    w_out.key("trace").begin_obj();

    w_out.key("inputs").begin_obj();
    w_out.key("word_in_dict").boolean(word_in_dict);
    w_out.key("power_percent").num(power);
    w_out.key("powered").boolean(use_powered);
    w_out.key("gems_enabled").boolean(gems_enabled);
    w_out.key("treasure_equipped").boolean(c.treasure_equipped);
    if (weakness_cat >= 0 && weakness_cat < NUM_BONUS_CATS)
    {
        w_out.key("active_weakness_category").str(CAT_NAMES[weakness_cat]);
        w_out.key("active_weakness_boost").num(c.active_weakness_boost);
    }
    else
    {
        w_out.key("active_weakness_category").str("none");
        w_out.key("active_weakness_boost").num(1.0);
    }
    w_out.key("base_damage_bonus").num(base_damage_bonus);
    w_out.key("enemy_armour").num(armour);
    w_out.end_obj();

    w_out.key("tiles").begin_arr();
    for (const auto& t : tiles) {
        w_out.begin_obj();
        std::string letter; letter += t.getLetter();
        w_out.key("letter").str(letter);
        w_out.key("points").num(t.getPoints());
        std::string gem_name = "none";
        switch (t.getGem())
        {
            case Gem::NONE: gem_name = "none"; break;
            case Gem::AMETHYST: gem_name = "amethyst"; break;
            case Gem::EMERALD: gem_name = "emerald"; break;
            case Gem::SAPPHIRE: gem_name = "sapphire"; break;
            case Gem::GARNET: gem_name = "garnet"; break;
            case Gem::RUBY: gem_name = "ruby"; break;
            case Gem::CRYSTAL: gem_name = "crystal"; break;
            case Gem::DIAMOND: gem_name = "diamond"; break;
        }
        w_out.key("gem").str(gem_name);
        w_out.key("gem_power").num(gemPower(t.getGem()));
        w_out.end_obj();
    }
    w_out.end_arr();

    w_out.key("bonus_membership").begin_obj();
    for (int cat = 0; cat < NUM_BONUS_CATS; ++cat)
    {
        const bool member = (finish_mask & bonus_bit_for(cat)) != 0;
        w_out.key(CAT_NAMES[cat]).boolean(member);
    }
    w_out.end_obj();

    w_out.key("multipliers_applied").begin_obj();
    w_out.key("power_boost").num(power_boost);
    w_out.key("treasure_boost").num(treasure_boost);
    w_out.key("treasure_applies").boolean(treasure_applies);
    w_out.key("weakness_boost").num(weakness_boost);
    w_out.key("weakness_applies").boolean(word_in_weakness);
    w_out.end_obj();

    w_out.key("formula").begin_obj();
    w_out.key("raw_points").num(raw_points);
    w_out.key("clamped_points").num(clamped);
    w_out.key("Q").num(Q);
    w_out.key("power_factor").num(power_factor);
    w_out.key("Q_after_power").num(q_after_power);
    w_out.key("gem_sum").num(gem_sum);
    w_out.key("gem_contribution_ceil").num(gem_contribution);
    w_out.key("inner").num(inner);
    w_out.key("after_treasure").num(after_treasure);
    w_out.key("after_power_boost").num(after_power_boost);
    w_out.key("after_base_damage").num(after_base);
    w_out.key("after_weakness").num(after_weakness);
    w_out.key("floored").num(floored);
    w_out.key("after_armour").num(after_armour);
    w_out.key("final_damage").num(final_damage);
    w_out.end_obj();

    w_out.key("verification").begin_obj();
    w_out.key("via_compute_damage").num(via_function);
    w_out.key("matches").boolean(via_function == final_damage);
    w_out.end_obj();

    w_out.end_obj();
    w_out.end_obj();

    return w_out.take();
}

template <int TrieSize>
inline std::string handle_best(const Trie<TrieSize>& trie, const jmini::Value& msg)
{
    auto* rack_v = msg.find("rack");
    if (!rack_v || !rack_v->is_string())
        return jmini::Writer().begin_obj().key("op").str("best").key("error").str("rack required").end_obj().take();

    const std::string& letters = rack_v->str();
    std::string gems_str;
    if (auto* g = msg.find("gems"); g && g->is_string())
        gems_str = g->str();

    Rack rack(letters, parse_gems_string(gems_str, letters.size()));
    const RuntimeConfig& cfg = config();
    const SearchResult sr = rack.bestWord(trie, cfg.power, cfg.powered);

    jmini::Writer w;
    w.begin_obj();
    w.key("op").str("best");
    std::string word_str;
    for (const auto& t : sr.tiles)
        word_str += t.getLetter();
    w.key("word").str(word_str);
    write_tiles(w, sr.tiles);
    w.key("damage").num(sr.damage);
    w.end_obj();
    return w.take();
}

template <int TrieSize>
inline std::string handle_top(
    const Trie<TrieSize>& trie, const jmini::Value& msg, BestWordCache& cache, std::mt19937& rng
)
{
    auto* rack_v = msg.find("rack");
    if (!rack_v || !rack_v->is_string())
    {
        return jmini::Writer().begin_obj().key("op").str("top").key("error").str("rack required").end_obj().take();
    }

    const std::string& letters = rack_v->str();
    std::string gems_str;
    if (auto* g = msg.find("gems");g && g->is_string())
        gems_str = g->str();

    int n = 20;
    int horizon = 2;
    int min_sims = 50;
    int max_sims = 300;
    double se_target = 0.5;
    int threshold = 0;
    double alpha = 0.2;
    double beta = 0.2;
    int max_kill_candidates = 500;
    int charges = 0;
    if (auto* v = msg.find("n"); v && v->is_number())
        n = static_cast<int>(v->num());
    if (auto* v = msg.find("horizon"); v && v->is_number())
        horizon = static_cast<int>(v->num());
    if (auto* v = msg.find("min_sims"); v && v->is_number())
        min_sims = static_cast<int>(v->num());
    if (auto* v = msg.find("max_sims"); v && v->is_number())
        max_sims = static_cast<int>(v->num());
    if (auto* v = msg.find("se_target"); v && v->is_number())
        se_target = v->num();
    if (auto* v = msg.find("threshold"); v && v->is_number())
        threshold = static_cast<int>(v->num());
    if (auto* v = msg.find("alpha"); v && v->is_number())
        alpha = v->num();
    if (auto* v = msg.find("beta"); v && v->is_number())
        beta = v->num();
    if (auto* v = msg.find("max_kill_candidates"); v && v->is_number())
        max_kill_candidates = static_cast<int>(v->num());
    if (auto* v = msg.find("charges"); v && v->is_number())
        charges = static_cast<int>(v->num());

    Rack rack(letters, parse_gems_string(gems_str, letters.size()));
    const RuntimeConfig& cfg = config();

    const double charge_cost = (charges > 0) ? (10.0 / charges) : 0.0;
    const bool dual_regime = (charges > 0);

    struct Eval
    {
        ScoredWord sw;
        double future;
        double total;
        int n_sims;
        bool is_kill;
        bool used_powered;
    };

    const auto rng_snapshot = rng;

    auto evaluate = [&](const ScoredWord& sw_unpowered, std::mt19937& rng_local, BestWordCache& cache_local) -> Eval {
        rng_local = rng_snapshot;
        Rack residual = rack;
        residual.playWord(Word(sw_unpowered.tiles));
        int n_sims = 0;
        const double future = monteCarloRackValue(
            residual, trie, horizon, min_sims, max_sims, se_target, rng_local, cache_local, &n_sims,
            cfg.power, false
        );

        Word w(sw_unpowered.tiles);
        const double raw_points = w.getPoints();
        double gem_sum = 0.0;
        if (cfg.gems_enabled)
        {
            for (const auto& t : sw_unpowered.tiles)
                gem_sum += gemPower(t.getGem());
        }

        std::uint8_t fm = 0;
        {
            int curr = TrieSize > 0 ? Trie<TrieSize>::ROOT : 0;
            bool ok = true;
            for (const auto& t : sw_unpowered.tiles)
            {
                if (t.isWildcard())
                {
                    ok = false;
                    break;
                }
                const int letter_idx = t.getLetter() - 'A';
                const int next = trie.child_for_letter(curr, letter_idx);
                if (next < 0)
                {
                    ok = false;
                    break;
                }
                curr = next;
            }
            if (ok)
                fm = trie.nodes[curr].finish_mask;
        }
        const double w_treasure = (cfg.treasure_equipped && (fm & bonus_bit_for(0))) ? 1.5 : 1.0;
        const double w_weakness =
            (cfg.active_weakness_cat >= 0 && (fm & bonus_bit_for(cfg.active_weakness_cat)))
                ? cfg.active_weakness_boost
                : 1.0;

        const int dmg_unp = compute_damage(
            raw_points, gem_sum, cfg.power, cfg.gems_enabled,
            1.0, w_treasure, w_weakness,
            cfg.base_damage_bonus, cfg.enemy_armour
        );
        const int dmg_pow = dual_regime
            ? compute_damage(
                raw_points, gem_sum, cfg.power, cfg.gems_enabled,
                1.25, w_treasure, w_weakness,
                cfg.base_damage_bonus, cfg.enemy_armour
              )
            : dmg_unp;

        const int length = static_cast<int>(sw_unpowered.tiles.size());
        const bool kill_unp = (threshold > 0) && (dmg_unp >= threshold);
        const bool kill_pow = (threshold > 0) && (dmg_pow >= threshold);

        auto score_for = [&](int damage, bool is_kill, bool is_powered) -> double {
            double s;
            if (is_kill)
            {
                const int overkill_bit = (damage - threshold >= 8) ? 1 : 0;
                s = (1.0 - alpha * overkill_bit) * future - beta * length;
            }
            else
            {
                s = damage + future;
            }
            if (is_powered)
                s -= charge_cost;
            return s;
        };

        const double score_unp = score_for(dmg_unp, kill_unp, false);
        const double score_pow = dual_regime
            ? score_for(dmg_pow, kill_pow, true)
            : -std::numeric_limits<double>::infinity();

        bool use_powered;
        if (threshold > 0 && (kill_unp || kill_pow))
        {
            if (kill_unp && kill_pow)
                use_powered = (score_pow > score_unp);
            else
                use_powered = kill_pow;
        }
        else
        {
            use_powered = (score_pow > score_unp);
        }

        ScoredWord chosen = sw_unpowered;
        chosen.damage = use_powered ? dmg_pow : dmg_unp;
        const bool is_kill = use_powered ? kill_pow : kill_unp;
        const double total = use_powered ? score_pow : score_unp;
        return Eval{chosen, future, total, n_sims, is_kill, use_powered};
    };

    auto run_evals = [&](std::vector<ScoredWord>& candidates, std::vector<Eval>& out) {
        const std::size_t N = candidates.size();
        out.resize(N);
        if (N == 0)
            return;

        constexpr std::size_t PARALLEL_CUTOFF = 16;
        unsigned int hw_threads = std::thread::hardware_concurrency();
        if (const char* env = std::getenv("SERVE_THREADS"))
        {
            const int v = std::atoi(env);
            if (v > 0)
                hw_threads = static_cast<unsigned int>(v);
        }
        const unsigned int desired_threads = hw_threads > 0 ? std::min(hw_threads, 8u) : 4u;
        const bool parallel = (N >= PARALLEL_CUTOFF) && (desired_threads >= 2);

        if (!parallel)
        {
            for (std::size_t i = 0; i < N; ++i)
                out[i] = evaluate(candidates[i], rng, cache);
            return;
        }

        const unsigned int n_threads = std::min<unsigned int>(
            desired_threads,
            static_cast<unsigned int>((N + 3) / 4)
        );

        std::atomic<std::size_t> next{0};
        auto worker = [&]() {
            std::mt19937 rng_local;
            BestWordCache cache_local(MAX_CACHE_SIZE);
            while (true)
            {
                const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= N)
                    break;
                out[idx] = evaluate(candidates[idx], rng_local, cache_local);
            }
        };

        std::vector<std::jthread> threads;
        threads.reserve(n_threads);
        for (unsigned int t = 0; t < n_threads; ++t)
            threads.emplace_back(worker);
        threads.clear();
    };

    std::vector<Eval> kill_evals;
    std::vector<Eval> nonkill_evals;

    auto tile_key = [](const ScoredWord& w) {
        std::string k; k.reserve(w.tiles.size() * 2);
        for (const auto& t : w.tiles)
        {
            k.push_back(t.getLetter());
            k.push_back(static_cast<char>(t.getGem()));
        }
        return k;
    };

    auto collect_unique = [&](std::vector<ScoredWord>& pool, std::vector<ScoredWord> more) {
        std::unordered_set<std::string> seen;
        seen.reserve(pool.size() + more.size());
        for (const auto& w : pool)
            seen.insert(tile_key(w));
        for (auto& w : more)
        {
            auto k = tile_key(w);
            if (seen.insert(std::move(k)).second)
                pool.push_back(std::move(w));
        }
    };

    std::vector<ScoredWord> all_candidates;

    if (threshold > 0)
    {
        auto kills_unp = rack.generateKills(trie, threshold, max_kill_candidates, cfg.power, false);
        all_candidates = std::move(kills_unp);
        if (dual_regime)
        {
            auto kills_pow = rack.generateKills(trie, threshold, max_kill_candidates, cfg.power, true);
            collect_unique(all_candidates, std::move(kills_pow));
        }
    }

    {
        ScoredHeap heap_unp = rack.generateWordlist(trie, n, cfg.power, false);
        std::vector<ScoredWord> all_top;
        all_top.reserve(heap_unp.size());
        while (!heap_unp.empty())
        {
            all_top.push_back(heap_unp.top());
            heap_unp.pop();
        }
        if (dual_regime)
        {
            ScoredHeap heap_pow = rack.generateWordlist(trie, n, cfg.power, true);
            std::vector<ScoredWord> more;
            more.reserve(heap_pow.size());
            while (!heap_pow.empty())
            {
                more.push_back(heap_pow.top());
                heap_pow.pop();
            }
            collect_unique(all_top, std::move(more));
        }

        constexpr std::size_t LINEAR_DEDUP_CUTOFF = 32;
        const std::size_t start_size = all_candidates.size();
        if (start_size * all_top.size() <= LINEAR_DEDUP_CUTOFF * LINEAR_DEDUP_CUTOFF && all_top.size() <= LINEAR_DEDUP_CUTOFF)
        {
            for (const ScoredWord& sw : all_top)
            {
                bool dup = false;
                for (const auto& c : all_candidates)
                {
                    if (c.tiles == sw.tiles)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                    all_candidates.push_back(sw);
            }
        }
        else
        {
            std::unordered_set<std::string> seen_keys;
            seen_keys.reserve(start_size + all_top.size());
            for (const auto& c : all_candidates)
                seen_keys.insert(tile_key(c));
            for (const ScoredWord& sw : all_top)
            {
                auto k = tile_key(sw);
                if (seen_keys.insert(std::move(k)).second)
                    all_candidates.push_back(sw);
            }
        }
    }

    std::vector<Eval> all_evals;
    run_evals(all_candidates, all_evals);

    kill_evals.reserve(all_evals.size());
    nonkill_evals.reserve(all_evals.size());
    for (auto& e : all_evals)
    {
        if (e.is_kill)
            kill_evals.push_back(std::move(e));
        else
            nonkill_evals.push_back(std::move(e));
    }

    std::sort(kill_evals.begin(), kill_evals.end(), [](const Eval& a, const Eval& b) {
        return a.total > b.total;
    });
    std::sort(nonkill_evals.begin(), nonkill_evals.end(), [](const Eval& a, const Eval& b) {
        return a.total > b.total;
    });

    jmini::Writer w;
    w.begin_obj();
    w.key("op").str("top");
    if (threshold > 0)
    {
        w.key("threshold").num(threshold);
        w.key("n_kills").num(static_cast<int>(kill_evals.size()));
    }
    if (dual_regime)
        w.key("charges").num(charges);
    w.key("words").begin_arr();
    auto emit = [&](const Eval& e) {
        w.begin_obj();
        std::string ws;
        for (const auto& t : e.sw.tiles)
            ws += t.getLetter();
        w.key("word").str(ws);
        w.key("now").num(e.sw.damage);
        w.key("future").num(e.future);
        w.key("total").num(e.total);
        w.key("n_sims").num(e.n_sims);
        w.key("is_kill").boolean(e.is_kill);
        w.key("used_powered").boolean(e.used_powered);
        write_tiles(w, e.sw.tiles);
        w.end_obj();
    };
    for (const auto& e : kill_evals)
        emit(e);
    for (const auto& e : nonkill_evals)
        emit(e);
    w.end_arr();
    w.end_obj();
    return w.take();
}

template <int TrieSize>
inline void run(const Trie<TrieSize>& trie)
{
    rebuild_search_tables();
    BestWordCache cache(MAX_CACHE_SIZE);
    std::mt19937 rng{std::random_device{}()};

    std::cerr << "wordgame: ready\n" << std::flush;

    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;
        std::string response;
        std::string op = "unknown";
        try
        {
            jmini::Parser p(line);
            jmini::Value msg = p.parse();
            if (auto* v = msg.find("op"); v && v->is_string())
                op = v->str();

            if (op == "ping")
            {
                response = jmini::Writer().begin_obj().key("op").str("pong").end_obj().take();
            }
            else if (op == "config")
            {
                response = handle_config(msg);
            }
            else if (op == "best")
            {
                response = handle_best(trie, msg);
            }
            else if (op == "trace")
            {
                response = handle_trace(trie, msg);
            }
            else if (op == "top")
            {
                response = handle_top(trie, msg, cache, rng);
            }
            else if (op == "quit")
            {
                std::cout << jmini::Writer().begin_obj().key("op").str("bye")
                                            .end_obj().take() << '\n' << std::flush;
                break;
            }
            else
            {
                response = jmini::Writer().begin_obj().key("op").str(op).key("error").str("unknown op")
                                          .end_obj().take();
            }
        }
        catch (const std::exception& e)
        {
            response = jmini::Writer().begin_obj().key("op").str(op).key("error").str(e.what()).end_obj().take();
        }
        std::cout << response << '\n' << std::flush;
    }
}

} // namespace serve
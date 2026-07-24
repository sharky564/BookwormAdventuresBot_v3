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


// Greedy per-turn policy used by the simulation harness. Mirrors the
// production `top` scoring (kill-aware future scaled to damage units, minus
// beta*length and the overkill delay), considering both unpowered and (when
// charges are available) powered plays. Returns {tiles, is_kill, used_powered};
// empty tiles if no word.
struct SimChoice { TileList tiles; bool is_kill; bool used_powered; };

template <int TrieSize>
inline SimChoice sim_pick_best(
    const Trie<TrieSize>& trie, const Rack& rack, const RuntimeConfig& cfg,
    int threshold, int next_threshold, bool terminal,
    double beta, double overkill_delay_penalty,
    double gamma, double margin,
    int horizon, int min_sims, int max_sims, double se_target,
    int charges, double charge_cost, bool use_plain_future,
    std::mt19937& rng, BestWordCache& cache)
{
    // Candidate pool: killing words (tight) + general top words. Include
    // powered kill candidates too when charges are available, since a powered
    // play can cross a kill threshold an unpowered one can't.
    std::vector<ScoredWord> cands = rack.generateKills(trie, threshold > 0 ? threshold : 1,
                                                        300, cfg.power, false);
    if (charges > 0)
    {
        auto pw = rack.generateKills(trie, threshold > 0 ? threshold : 1, 300, cfg.power, true);
        for (auto& x : pw) cands.push_back(std::move(x));
    }
    {
        ScoredHeap heap = rack.generateWordlist(trie, 20, cfg.power, false);
        while (!heap.empty()) { cands.push_back(heap.top()); heap.pop(); }
    }
    if (cands.empty())
        return {TileList{}, false, false};

    double best_score = -1e18;
    const ScoredWord* best = nullptr;
    bool best_kill = false;
    bool best_powered = false;

    for (const auto& sw : cands)
    {
        Word w(sw.tiles);
        const double raw_points = w.getPoints();
        double gem_sum = 0.0;
        if (cfg.gems_enabled)
            for (const auto& t : sw.tiles) gem_sum += gemPower(t.getGem());

        std::uint8_t fm = 0;
        {
            int curr = TrieSize > 0 ? Trie<TrieSize>::ROOT : 0;
            bool ok = true;
            for (const auto& t : sw.tiles)
            {
                if (t.isWildcard()) { ok = false; break; }
                const int li = t.getLetter() - 'A';
                const int nx = trie.child_for_letter(curr, li);
                if (nx < 0) { ok = false; break; }
                curr = nx;
            }
            if (ok) fm = trie.nodes[curr].finish_mask;
        }
        const double w_tr = (cfg.treasure_equipped && (fm & bonus_bit_for(0))) ? 1.5 : 1.0;
        const double w_wk = (cfg.active_weakness_cat >= 0 && (fm & bonus_bit_for(cfg.active_weakness_cat)))
                            ? cfg.active_weakness_boost : 1.0;
        const int length = static_cast<int>(sw.tiles.size());

        // Evaluate both unpowered and (if charges remain) powered regimes.
        const int n_regimes = (charges > 0) ? 2 : 1;
        for (int r = 0; r < n_regimes; ++r)
        {
            const bool powered = (r == 1);
            const int dmg = compute_damage(raw_points, gem_sum, cfg.power, cfg.gems_enabled,
                                           powered ? 1.25 : 1.0, w_tr, w_wk,
                                           cfg.base_damage_bonus, cfg.enemy_armour);
            const bool is_kill = (threshold > 0) && (dmg >= threshold);

            double score;
            if (terminal)
            {
                const int excess = dmg - threshold;
                const double delay = (excess >= 8) ? overkill_delay_penalty : 0.0;
                score = is_kill ? (-beta * length - delay) : static_cast<double>(dmg);
            }
            else
            {
                Rack residual = rack;
                residual.playWord(Word(sw.tiles));
                int rt; Gem drop = Gem::NONE;
                if (is_kill) { rt = next_threshold; const int ex = dmg - threshold; drop = overkill_gem_for_excess(ex); }
                else { rt = (threshold > 0) ? std::max(0, threshold - dmg) : 0; }
                int ns = 0;
                double fut = monteCarloRackValue(residual, trie, horizon, min_sims, max_sims,
                                                 se_target, rng, cache, &ns, cfg.power, false,
                                                 rt, drop, gamma, margin, use_plain_future);
                // The kill-aware metric returns a [0,1]-ish kill value that we
                // scale into damage units by the threshold it targets. The
                // plain-damage rollout already returns damage units, so it is
                // NOT scaled.
                if (!use_plain_future && rt > 0) fut *= static_cast<double>(rt);
                if (is_kill)
                {
                    const int excess = dmg - threshold;
                    const double delay = (excess >= 8) ? overkill_delay_penalty : 0.0;
                    score = fut - beta * length - delay;
                }
                else
                    score = dmg + fut;
            }
            if (powered) score -= charge_cost;  // spending a charge has a cost

            if (score > best_score)
            {
                best_score = score; best = &sw; best_kill = is_kill; best_powered = powered;
            }
        }
    }
    if (!best) return {TileList{}, false, false};
    return {best->tiles, best_kill, best_powered};
}

template <int TrieSize>
inline std::string handle_simulate(
    const Trie<TrieSize>& trie, const jmini::Value& msg
)
{
    // Honest-play simulation of a chapter, run entirely in C++ for speed.
    // Request fields:
    //   config: a config-op-style object applied to all enemies in the chapter
    //           (letter_points, gems_enabled, prescramble, power is per-enemy)
    //   enemies: [{hp, armour, power, weakness_cat?, weakness_boost?,
    //             base_damage_bonus?, treasure?, terminal?}, ...]
    //             (hp/threshold are already in quarter-heart units = hearts*4)
    //   seeds, max_sims, horizon, beta, overkill_delay_penalty,
    //   kill_gamma, kill_margin, turn_cap, base_seed
    // Response: {"op":"simulate","mean_turns":..,"fail_rate":..,"seeds":N}
    auto* enemies_v = msg.find("enemies");
    if (!enemies_v || !enemies_v->is_array())
        return jmini::Writer().begin_obj().key("op").str("simulate")
            .key("error").str("enemies (array) required").end_obj().take();

    // Apply chapter-wide config (letter_points, gems_enabled, prescramble).
    RuntimeConfig base_cfg = config();
    if (auto* cv = msg.find("config"); cv && cv->is_object())
    {
        if (auto* v = cv->find("gems_enabled"); v && v->is_bool())
            base_cfg.gems_enabled = v->boolean();
        if (auto* v = cv->find("prescramble"); v && v->is_bool())
            base_cfg.prescramble = v->boolean();
        if (auto* v = cv->find("letter_points"); v && v->is_array())
        {
            const auto& a = v->arr();
            for (std::size_t i = 0; i < 26 && i < a.size(); ++i)
                if (a[i].is_number()) base_cfg.letter_points[i] = a[i].num();
        }
    }

    int seeds = 40, max_sims = 60, horizon = 2, min_sims = 30, turn_cap = 40;
    double se_target = 0.5, beta = 0.2, delay = 2.0, gamma = 0.7, margin = 8.0;
    std::uint64_t base_seed = 1;
    auto getn = [&](const char* k, auto& dst) {
        if (auto* v = msg.find(k); v && v->is_number())
            dst = static_cast<std::decay_t<decltype(dst)>>(v->num());
    };
    getn("seeds", seeds); getn("max_sims", max_sims); getn("horizon", horizon);
    getn("min_sims", min_sims); getn("turn_cap", turn_cap); getn("se_target", se_target);
    getn("beta", beta); getn("overkill_delay_penalty", delay);
    getn("kill_gamma", gamma); getn("kill_margin", margin); getn("base_seed", base_seed);

    // future_metric: "kill" (default, kill-aware) or "plain" (sum-of-damage).
    bool use_plain_future = false;
    if (auto* v = msg.find("future_metric"); v && v->is_string())
        use_plain_future = (v->str() == "plain");

    // Parse the enemy list.
    struct Enemy { int hp4; int armour; double power; int wk_cat; double wk_boost;
                   int base_bonus; bool treasure; bool terminal; };
    std::vector<Enemy> enemies;
    for (const auto& ev : enemies_v->arr())
    {
        if (!ev.is_object()) continue;
        Enemy en{0,0,0.0,-1,1.0,0,false,false};
        if (auto* v = ev.find("hp"); v && v->is_number()) en.hp4 = static_cast<int>(v->num());
        if (auto* v = ev.find("armour"); v && v->is_number()) en.armour = static_cast<int>(v->num());
        if (auto* v = ev.find("power"); v && v->is_number()) en.power = v->num();
        if (auto* v = ev.find("weakness_cat"); v && v->is_number()) en.wk_cat = static_cast<int>(v->num());
        if (auto* v = ev.find("weakness_boost"); v && v->is_number()) en.wk_boost = v->num();
        if (auto* v = ev.find("base_damage_bonus"); v && v->is_number()) en.base_bonus = static_cast<int>(v->num());
        if (auto* v = ev.find("treasure"); v && v->is_bool()) en.treasure = v->boolean();
        if (auto* v = ev.find("terminal"); v && v->is_bool()) en.terminal = v->boolean();
        enemies.push_back(en);
    }

    auto& dist = distribution();
    std::atomic<int> total_turns{0};
    std::atomic<int> fails{0};

    // Set the global config's CHAPTER-level fields once (letter_points,
    // gems_enabled, prescramble). These are read by generateKills /
    // generateWordlist / compute_damage and are constant across the whole
    // simulation, so it is safe for parallel workers to read them. Per-enemy
    // fields (power, armour, weakness, ...) are passed explicitly and never
    // touch the global, so there is no data race.
    config() = base_cfg;

    unsigned int hw = std::thread::hardware_concurrency();
    if (const char* env = std::getenv("SERVE_THREADS"))
    { const int v = std::atoi(env); if (v > 0) hw = static_cast<unsigned int>(v); }
    const unsigned int n_threads = std::max(1u, std::min(hw, 8u));

    std::atomic<int> next_seed{0};
    auto worker = [&]() {
        BestWordCache cache(MAX_CACHE_SIZE);
        while (true)
        {
            const int s = next_seed.fetch_add(1, std::memory_order_relaxed);
            if (s >= seeds) break;
            std::mt19937 rng(static_cast<std::mt19937::result_type>(base_seed + s));
            std::string letters;
            for (int i = 0; i < 16; ++i)
                letters += static_cast<char>('A' + dist(rng));
            Rack rack(letters, std::vector<Gem>(16, Gem::NONE));

            int turns = 0;
            int charges = 0;  // diamond-charge economy, persists across enemies
            bool cleared = true;
            for (std::size_t ei = 0; ei < enemies.size() && turns < turn_cap; ++ei)
            {
                const Enemy& en = enemies[ei];
                RuntimeConfig cfg = base_cfg;
                cfg.power = en.power;
                cfg.enemy_armour = en.armour;
                cfg.active_weakness_cat = en.wk_cat;
                cfg.active_weakness_boost = en.wk_boost;
                cfg.base_damage_bonus = en.base_bonus;
                cfg.treasure_equipped = en.treasure;

                const int threshold = en.hp4;
                int next_threshold = 0;
                if (!en.terminal && ei + 1 < enemies.size())
                    next_threshold = enemies[ei + 1].hp4;

                int remaining = threshold;
                bool killed = false;
                int enemy_turns = 0;
                while (!killed && turns < turn_cap)
                {
                    const double charge_cost = (charges > 0) ? (10.0 / charges) : 0.0;
                    auto choice = sim_pick_best(
                        trie, rack, cfg, remaining, next_threshold, en.terminal,
                        beta, delay, gamma, margin, horizon, min_sims, max_sims,
                        se_target, charges, charge_cost, use_plain_future, rng, cache);
                    const TileList& tiles = choice.tiles;
                    const bool powered = choice.used_powered;
                    ++turns; ++enemy_turns;
                    if (tiles.empty()) break;
                    Word w(tiles);
                    double gem_sum = 0.0;
                    if (cfg.gems_enabled)
                        for (const auto& t : tiles) gem_sum += gemPower(t.getGem());
                    std::uint8_t fm = 0;
                    {
                        int curr = TrieSize > 0 ? Trie<TrieSize>::ROOT : 0;
                        bool ok = true;
                        for (const auto& t : tiles)
                        {
                            if (t.isWildcard()) { ok = false; break; }
                            const int li = t.getLetter() - 'A';
                            const int nx = trie.child_for_letter(curr, li);
                            if (nx < 0) { ok = false; break; }
                            curr = nx;
                        }
                        if (ok) fm = trie.nodes[curr].finish_mask;
                    }
                    const double w_tr = (cfg.treasure_equipped && (fm & bonus_bit_for(0))) ? 1.5 : 1.0;
                    const double w_wk = (cfg.active_weakness_cat >= 0 && (fm & bonus_bit_for(cfg.active_weakness_cat)))
                                        ? cfg.active_weakness_boost : 1.0;
                    const int dmg = compute_damage(w.getPoints(), gem_sum, cfg.power, cfg.gems_enabled,
                                                   powered ? 1.25 : 1.0, w_tr, w_wk,
                                                   cfg.base_damage_bonus, cfg.enemy_armour);
                    remaining -= dmg;
                    const bool this_kill = (remaining <= 0);

                    // Apply the word and refill. Use the gem-generating overload
                    // so words spawn their expected gem on the next rack
                    // (gems-from-words). Then, if this play overkilled by >= 8,
                    // drop the overkill gem onto a random next-rack tile
                    // (gems-from-overkills) -- realizing what the lookahead values.
                    rack.playWord(w, rng);
                    if (this_kill)
                    {
                        const int excess = -remaining;  // dmg beyond what was needed
                        const Gem og = overkill_gem_for_excess(excess);
                        if (og != Gem::NONE)
                            rack.dropGemOnRandomTile(og, rng);
                    }

                    // Charge economy: each diamond consumed by THIS word adds a
                    // charge; a powered play spends one.
                    for (const auto& t : tiles)
                        if (t.getGem() == Gem::DIAMOND) ++charges;
                    if (powered && charges > 0) --charges;

                    if (this_kill) killed = true;
                    if (enemy_turns > turn_cap) break;
                }
                if (!killed) { cleared = false; break; }
            }
            if (!cleared || turns >= turn_cap) { fails.fetch_add(1); total_turns.fetch_add(turn_cap); }
            else total_turns.fetch_add(turns);
        }
    };

    {
        std::vector<std::jthread> threads;
        for (unsigned int t = 0; t < n_threads; ++t)
            threads.emplace_back(worker);
    }

    jmini::Writer w;
    w.begin_obj();
    w.key("op").str("simulate");
    w.key("mean_turns").num(static_cast<double>(total_turns.load()) / std::max(1, seeds));
    w.key("fail_rate").num(static_cast<double>(fails.load()) / std::max(1, seeds));
    w.key("seeds").num(seeds);
    w.end_obj();
    return w.take();
}


template <int TrieSize>
inline std::string handle_step(
    const Trie<TrieSize>& trie, const jmini::Value& msg
)
{
    // Apply a word to a rack and refill the consumed tiles using the real
    // tile-frequency distribution, returning the resulting rack. Used by the
    // honest-play simulation harness to advance one turn while keeping the
    // refill model authoritative (and reproducible via an explicit seed).
    //
    // Request:  {"op":"step","rack":..,"gems":..,"word":"TILES",
    //            "refill_gem":"none","seed":12345}
    // Response: {"op":"step","rack":"NEWLETTERS","gems":"0..."}
    (void)trie;
    auto* rack_v = msg.find("rack");
    auto* word_v = msg.find("word");
    if (!rack_v || !rack_v->is_string() || !word_v || !word_v->is_string())
        return jmini::Writer().begin_obj().key("op").str("step")
            .key("error").str("rack and word required").end_obj().take();

    const std::string& letters = rack_v->str();
    std::string gems_str;
    if (auto* g = msg.find("gems"); g && g->is_string())
        gems_str = g->str();

    std::uint64_t seed = 0;
    if (auto* v = msg.find("seed"); v && v->is_number())
        seed = static_cast<std::uint64_t>(v->num());

    Gem refill_gem = Gem::NONE;
    bool refill_wild = false;

    Rack rack(letters, parse_gems_string(gems_str, letters.size()));

    // Build the played Word from the rack tiles matching the word's letters.
    const std::string& word_str = word_v->str();
    TileList played;
    {
        // greedily match each word letter to an available rack tile
        std::vector<bool> used(rack.getTiles().size(), false);
        for (char wc : word_str)
        {
            for (std::size_t i = 0; i < rack.getTiles().size(); ++i)
            {
                if (used[i]) continue;
                if (rack.getTiles()[i].getLetter() == wc)
                {
                    played.push_back(rack.getTiles()[i]);
                    used[i] = true;
                    break;
                }
            }
        }
    }

    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    Word w(played);
    rack.playWord(w);
    // Generate the word's expected gem on refill (gems-from-words), matching
    // the real mechanic.
    refill_gem = w.expectedGem();
    refill_wild = w.checkWildcard();
    rack.regenerateTiles(refill_gem, refill_wild, rng);

    std::string out_letters;
    std::string out_gems;
    for (const auto& t : rack.getTiles())
    {
        out_letters += t.getLetter();
        out_gems += static_cast<char>('0' + static_cast<int>(t.getGem()));
    }

    jmini::Writer wr;
    wr.begin_obj();
    wr.key("op").str("step");
    wr.key("rack").str(out_letters);
    wr.key("gems").str(out_gems);
    wr.end_obj();
    return wr.take();
}

template <int TrieSize>
inline std::string handle_replay(
    const Trie<TrieSize>& trie, const jmini::Value& msg
)
{
    // Fixed-rack "replay" solve: one rack is played repeatedly against a block
    // of enemies (the 1.7 / 3.10 menu-skip glitch). For each effective
    // threshold supplied, return the tightest-killing words -- those whose
    // damage meets the threshold with the LEAST overkill -- so the caller can
    // pick one word per distinct enemy-HP tier and minimise wasted overkill
    // across the block.
    //
    // Effective threshold convention: the caller folds armour in, i.e. for an
    // enemy with HP h and armour a it passes (h + a), and damage here is
    // computed with armour = 0. This keeps the op armour-agnostic.
    //
    // Request:  {"op":"replay","rack":..,"gems":..,"thresholds":[t1,t2,..],
    //            "per_threshold":3,"max_candidates":500}
    // Response: {"op":"replay","results":[{"threshold":t,"words":[...]},..]}
    auto* rack_v = msg.find("rack");
    if (!rack_v || !rack_v->is_string())
        return jmini::Writer().begin_obj().key("op").str("replay")
            .key("error").str("rack required").end_obj().take();

    auto* thr_v = msg.find("thresholds");
    if (!thr_v || !thr_v->is_array())
        return jmini::Writer().begin_obj().key("op").str("replay")
            .key("error").str("thresholds (array) required").end_obj().take();

    const std::string& letters = rack_v->str();
    std::string gems_str;
    if (auto* g = msg.find("gems"); g && g->is_string())
        gems_str = g->str();

    int per_threshold = 3;
    int max_candidates = 500;
    if (auto* v = msg.find("per_threshold"); v && v->is_number())
        per_threshold = std::max(1, static_cast<int>(v->num()));
    if (auto* v = msg.find("max_candidates"); v && v->is_number())
        max_candidates = static_cast<int>(v->num());

    // Collect requested thresholds.
    std::vector<int> thresholds;
    for (const auto& t : thr_v->arr())
        if (t.is_number())
            thresholds.push_back(static_cast<int>(t.num()));

    const RuntimeConfig& cfg = config();
    Rack rack(letters, parse_gems_string(gems_str, letters.size()));

    // Generate kill candidates once at the LOWEST threshold (a superset: any
    // word that kills a higher threshold also kills a lower one). Powered and
    // unpowered both, deduplicated. Each candidate's damage is then compared
    // against each requested threshold.
    int min_threshold = thresholds.empty() ? 1 : thresholds[0];
    for (int t : thresholds)
        min_threshold = std::min(min_threshold, t);
    if (min_threshold < 1)
        min_threshold = 1;

    const bool dual = (msg.find("charges") != nullptr);  // allow powered variants if charges present
    auto candidates = rack.generateKills(trie, min_threshold, max_candidates, cfg.power, false);
    if (dual)
    {
        auto powered = rack.generateKills(trie, min_threshold, max_candidates, cfg.power, true);
        for (auto& w : powered)
            candidates.push_back(std::move(w));
    }

    jmini::Writer w;
    w.begin_obj();
    w.key("op").str("replay");
    w.key("results").begin_arr();
    for (int t : thresholds)
    {
        // Words that kill this threshold, sorted by ascending overkill
        // (damage - t), then by ascending length (shorter = faster).
        std::vector<const ScoredWord*> killers;
        for (const auto& c : candidates)
            if (c.damage >= t)
                killers.push_back(&c);
        std::sort(killers.begin(), killers.end(),
            [t](const ScoredWord* a, const ScoredWord* b) {
                const int oa = a->damage - t, ob = b->damage - t;
                if (oa != ob) return oa < ob;
                return a->tiles.size() < b->tiles.size();
            });

        w.begin_obj();
        w.key("threshold").num(t);
        w.key("words").begin_arr();
        int emitted = 0;
        for (const ScoredWord* c : killers)
        {
            if (emitted >= per_threshold)
                break;
            w.begin_obj();
            std::string ws;
            for (const auto& tile : c->tiles)
                ws += tile.getLetter();
            w.key("word").str(ws);
            w.key("now").num(c->damage);
            w.key("overkill").num(c->damage - t);
            w.key("is_kill").boolean(true);
            write_tiles(w, c->tiles);
            w.end_obj();
            ++emitted;
        }
        w.end_arr();
        w.end_obj();
    }
    w.end_arr();
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
    int next_enemy_hp = 0;
    bool terminal = false;
    double alpha = 0.2;
    double beta = 0.2;
    double overkill_delay_penalty = 2.0;
    double kill_gamma = 0.7;
    double kill_margin = 8.0;
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
    if (auto* v = msg.find("next_enemy_hp"); v && v->is_number())
        next_enemy_hp = static_cast<int>(v->num());
    if (auto* v = msg.find("terminal"); v && v->is_bool())
        terminal = v->boolean();
    if (auto* v = msg.find("alpha"); v && v->is_number())
        alpha = v->num();
    if (auto* v = msg.find("beta"); v && v->is_number())
        beta = v->num();
    if (auto* v = msg.find("overkill_delay_penalty"); v && v->is_number())
        overkill_delay_penalty = v->num();
    if (auto* v = msg.find("kill_gamma"); v && v->is_number())
        kill_gamma = v->num();
    if (auto* v = msg.find("kill_margin"); v && v->is_number())
        kill_margin = v->num();
    (void)alpha;  // retained for protocol back-compat; no longer used in scoring
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

        // The MC future depends on the enemy we'll face NEXT turn, which
        // depends on whether THIS word kills the current enemy:
        //   - kills current enemy  -> next turn faces the next enemy (full HP)
        //   - doesn't kill         -> next turn faces the current enemy with
        //                             its remaining HP (threshold - damage)
        // When enemy-aware metric is active (next_threshold known), the future
        // is scaled back into damage units by multiplying the [0,1]-ish kill
        // value by the threshold it targets, so it stays comparable to the
        // damage-scale terms in the score. n_sims is captured from the most
        // expensive (kill-branch) rollout for reporting.
        int n_sims_captured = 0;

        auto future_for = [&](int damage, bool is_kill) -> double {
            // Terminal mode: there is no meaningful next rack (chapter ends in
            // a fixed-rack transition, or this is the final enemy). The future
            // term is identical across all killing words, so it cannot inform
            // word choice -- drop it and rank kills purely by economy
            // (shortest word, least overkill).
            if (terminal)
                return 0.0;

            int rollout_threshold;
            Gem drop_gem = Gem::NONE;
            if (is_kill)
            {
                rollout_threshold = next_enemy_hp;  // 0 if not supplied
                const int excess = damage - threshold;
                drop_gem = overkill_gem_for_excess(excess);
            }
            else
            {
                // Current enemy survives; next turn it has this much HP left.
                rollout_threshold = (threshold > 0) ? (threshold - damage) : 0;
                if (rollout_threshold < 0)
                    rollout_threshold = 0;
            }

            int ns = 0;
            const double raw_future = monteCarloRackValue(
                residual, trie, horizon, min_sims, max_sims, se_target,
                rng_local, cache_local, &ns, cfg.power, false,
                rollout_threshold, drop_gem, kill_gamma, kill_margin
            );
            if (ns > n_sims_captured)
                n_sims_captured = ns;

            // Scale kill-probability value into damage units when enemy-aware.
            if (rollout_threshold > 0)
                return raw_future * static_cast<double>(rollout_threshold);
            return raw_future;
        };

        auto score_for = [&](int damage, bool is_kill, bool is_powered, double& out_future) -> double {
            const double future = future_for(damage, is_kill);
            out_future = future;
            double s;
            if (is_kill)
            {
                // The future already reflects the overkill gem (credited in
                // the rollout). Lost overkill damage is implicit: a smaller
                // killing word leaves a stronger residual -> higher future.
                // The only explicit overkill cost is the longer kill
                // animation, charged as a flat damage-equivalent delay.
                const int excess = damage - threshold;
                const double delay = (excess >= 8) ? overkill_delay_penalty : 0.0;
                s = future - beta * length - delay;
            }
            else
            {
                s = damage + future;
            }
            if (is_powered)
                s -= charge_cost;
            return s;
        };

        double future_unp = 0.0;
        double future_pow = 0.0;
        const double score_unp = score_for(dmg_unp, kill_unp, false, future_unp);
        const double score_pow = dual_regime
            ? score_for(dmg_pow, kill_pow, true, future_pow)
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
        const double reported_future = use_powered ? future_pow : future_unp;
        return Eval{chosen, reported_future, total, n_sims_captured, is_kill, use_powered};
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
            else if (op == "replay")
            {
                response = handle_replay(trie, msg);
            }
            else if (op == "step")
            {
                response = handle_step(trie, msg);
            }
            else if (op == "simulate")
            {
                response = handle_simulate(trie, msg);
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
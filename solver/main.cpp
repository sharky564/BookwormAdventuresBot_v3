#include "class_scan.hpp"
#include "constants.hpp"
#include "mc.hpp"
#include "rack.hpp"
#include "serve.hpp"
#include "trie.hpp"
#include "utils.hpp"
#include "word.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void play_game(const Trie<NUM_WORDS>& trie)
{
    constexpr int HORIZON = 2;
    constexpr int MIN_SIMS = 50;
    constexpr int MAX_SIMS = 500;
    constexpr double SE_TARGET = 0.5;

    BestWordCache cache(MAX_CACHE_SIZE);
    FastRng rng{std::random_device{}()};

    std::cout << std::format(
        "Columns: Now = immediate damage, Future = E[next {} turns], "
        "Total = Now + Future (sorted by Total).\n\n", HORIZON);

    std::string input, gem_string;
    while (true)
    {
        std::cout << "Enter a rack (empty to quit): " << std::flush;
        if (!std::getline(std::cin, input))
            break;
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
            input.pop_back();
        if (input.empty())
            break;

        for (char& c : input)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (input.size() > static_cast<std::size_t>(MAX_RACK_SIZE))
        {
            std::cout << std::format("Rack too long ({} tiles, max {}).\n", input.size(), MAX_RACK_SIZE);
            continue;
        }

        gem_string.clear();
        if (config().gems_enabled)
        {
            std::cout << "Enter gems: " << std::flush;
            if (!std::getline(std::cin, gem_string))
                break;
        }
        gem_string.resize(input.size(), ' ');

        std::vector<Gem> gems;
        gems.reserve(gem_string.size());
        for (char c : gem_string)
            gems.push_back(translateGem(c));

        const Rack rack(input, gems);
        ScoredHeap top = rack.generateWordlist(trie, 20);

        std::vector<ScoredWord> words;
        words.reserve(top.size());
        while (!top.empty())
        {
            words.push_back(top.top());
            top.pop();
        }

        if (words.empty())
        {
            std::cout << "(no words found)\n";
            continue;
        }

        const auto rng_snapshot = rng;
        const auto h0 = cache.hits();
        const auto m0 = cache.misses();
        const auto t0 = std::chrono::steady_clock::now();

        struct Eval
        {
            ScoredWord sw;
            double future;
            double total;
            int n_sims;
        };
        std::vector<Eval> evals;
        evals.reserve(words.size());

        for (const ScoredWord& sw : words)
        {
            rng = rng_snapshot;
            Rack residual = rack;
            residual.playWord(Word(sw.tiles));
            int n = 0;
            const double future = monteCarloRackValue(
                residual, trie, HORIZON, MIN_SIMS, MAX_SIMS, SE_TARGET,
                rng, cache, &n);
            evals.push_back({sw, future, sw.damage + future, n});
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double eval_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const auto dh = cache.hits()   - h0;
        const auto dm = cache.misses() - m0;
        const double hit_pct = (dh + dm > 0) ? 100.0 * static_cast<double>(dh) / (dh + dm) : 0.0;

        std::sort(evals.begin(), evals.end(), [](const Eval& a, const Eval& b) { return a.total > b.total; });

        std::cout << std::format("\n{:<14} {:>5} {:>8} {:>8} {:>5}\n", "Word", "Now", "Future", "Total", "sims");
        std::cout << std::string(14 + 1 + 5 + 1 + 8 + 1 + 8 + 1 + 5, '-') << '\n';
        for (const auto& e : evals)
        {
            std::string wstr;
            wstr.reserve(e.sw.tiles.size());
            for (const auto& t : e.sw.tiles)
                wstr += t.getLetter();
            std::cout << std::format("{:<14} {:>5} {:>8.2f} {:>8.2f} {:>5}\n",
                                     wstr, e.sw.damage, e.future, e.total, e.n_sims);
            if (config().gems_enabled)
            {
                std::string gstr; gstr.reserve(e.sw.tiles.size());
                for (const auto& t : e.sw.tiles)
                    gstr += gemCharacter(t.getGem());
                std::cout << std::format("  gems: {}\n", gstr);
            }
        }
        std::cout << std::format("\n({:.0f}ms, {:.1f}% cache hit on this rack, {} entries total)\n\n",
                                 eval_ms, hit_pct, cache.size());
    }
}

void print_usage(const char* prog)
{
    std::cerr << std::format(
        "Usage:\n"
        "  {0} <dict.txt>                       # interactive play\n"
        "  {0} <dict.txt> serve                 # JSON IPC over stdin/stdout (for python)\n"
        "  {0} <dict.txt> gen-rack [N=100000]   # per-rack value data\n"
        "  {0} <dict.txt> gen-word [N=100000]   # per-(rack,word) Q-value data (for ML)\n",
        prog);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    auto trie = std::make_unique<Trie<NUM_WORDS>>();
    const std::string dict_path = argv[1];
    const std::string mode_preview = argc > 2 ? argv[2] : "play";
    const bool serve_mode = (mode_preview == "serve");
    if (!serve_mode)
        std::cout << std::format("Loading words into trie from {}...\n", dict_path) << std::flush;
    else
        std::cerr << std::format("Loading words into trie from {}...\n", dict_path) << std::flush;

    const auto result = trie->load(dict_path);
    if (!result)
    {
        std::cerr << std::format("Trie::load failed: {}\n",
            result.error() == TrieLoadError::FileNotFound ? "file not found" : "read error"
        ) << std::flush;
        return 1;
    }
    if (serve_mode)
        std::cerr << "main dict loaded.\n" << std::flush;

    std::string dict_dir;
    {
        const auto pos = dict_path.find_last_of("/\\");
        if (pos != std::string::npos)
            dict_dir = dict_path.substr(0, pos);
    }
    const std::array<std::pair<int, const char*>, NUM_BONUS_CATS> bonus_files = {{
        {0, "ba1-metals.txt"},
        {1, "ba1-cats.txt"},
        {2, "ba1-colors.txt"},
        {3, "ba1-bones.txt"},
        {4, "ba1-fruitandveg.txt"},
    }};
    for (const auto& [cat, fname] : bonus_files)
    {
        std::string path;
        if (!dict_dir.empty())
        {
            path = dict_dir;
            path += '/';
        }
        path += fname;
        if (serve_mode)
            std::cerr << std::format("checking bonus dict {}...\n", path) << std::flush;
        std::ifstream probe(path);
        if (!probe.good())
        {
            std::cerr << std::format(
                "  not found (category {} will be empty)\n", cat
            ) << std::flush;
            continue;
        }
        probe.close();
        int mismatches = 0;
        const auto br = trie->load_bonus(path, cat, &mismatches);
        if (!br)
        {
            std::cerr << std::format(
                "  load failed: {}\n",
                br.error() == TrieLoadError::FileNotFound ? "file not found" : "read error"
            ) << std::flush;
            continue;
        }
        std::cerr << std::format("  matched {} words", *br) << std::flush;
        if (mismatches > 0)
            std::cerr << std::format(" ({} mismatches)", mismatches) << std::flush;
        std::cerr << "\n" << std::flush;
    }

    if (serve_mode)
        std::cerr << "finalizing trie...\n" << std::flush;
    trie->finalize();
    if (serve_mode)
        std::cerr << "trie finalized.\n" << std::flush;
    rebuild_search_tables();
    if (serve_mode)
        std::cerr << "search tables built.\n" << std::flush;
    class_index().build(*trie);
    if (const char* env = std::getenv("SOLVER_ENGINE"); env && std::string_view(env) == "scan")
        search_engine() = SearchEngine::Scan;
    if (serve_mode)
        std::cerr << std::format("class index built ({} classes, {} variants).\n",
                                 class_index().num_classes(), class_index().num_variants()) << std::flush;

    const std::string mode = argc > 2 ? argv[2] : "play";

    if (mode == "serve")
    {
        serve::run(*trie);
        return 0;
    }

    std::cout << std::format("Loaded {} words. Node count: {}\n", *result, trie->node_count());

    if (mode == "gen-rack")
    {
        GenerateConfig cfg;
        if (argc > 3)
            cfg.num_racks = std::stoi(argv[3]);
        generateDataParallel(*trie, "rack_data.tsv", cfg);
    }
    else if (mode == "gen-word")
    {
        PerWordConfig cfg;
        if (argc > 3)
            cfg.num_racks = std::stoi(argv[3]);
        generatePerWordDataParallel(*trie, "word_data.tsv", cfg);
    }
    else if (mode == "play")
    {
        play_game(*trie);
    }
    else
    {
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}
#include "rack.hpp"
#include "class_scan.hpp"
#include <algorithm>
#include <array>

namespace {

template <typename DrawFn>
void regenerate_core(TileList& tiles, int target_size, Gem gem, bool wildcard, DrawFn draw_fn)
{
    std::array<int, 26> freq{};
    bool has_wildcard = false;
    for (const auto& t : tiles)
    {
        if (t.isWildcard())
            has_wildcard = true;
        else
            ++freq[t.getLetter() - 'A'];
    }

    bool first_added = false;
    auto consume_gem = [&]() -> Gem
    {
        if (first_added)
            return Gem::NONE;
        first_added = true;
        const Gem g = gem;
        gem = Gem::NONE;
        return g;
    };

    auto try_add = [&](char letter, Gem g) -> bool
    {
        if (letter == WILDCARD_CHAR)
        {
            if (has_wildcard)
                return false;
            tiles.emplace_back(WILDCARD_CHAR, Gem::NONE);
            has_wildcard = true;
            return true;
        }
        const int idx = letter - 'A';
        if (freq[idx] >= max_letter_frequency[idx])
            return false;
        tiles.emplace_back(letter, g);
        ++freq[idx];
        return true;
    };

    if (config().rainbow)
    {
        if (wildcard && static_cast<int>(tiles.size()) < target_size)
            try_add(WILDCARD_CHAR, Gem::NONE);
    }

    if (config().prescramble)
    {
        if (freq['A' - 'A'] == 0 && static_cast<int>(tiles.size()) < target_size)
            try_add('A', consume_gem());
        if (freq['E' - 'A'] == 0 && static_cast<int>(tiles.size()) < target_size)
            try_add('E', consume_gem());
    }

    while (static_cast<int>(tiles.size()) < target_size)
    {
        const int idx = draw_fn();
        if (freq[idx] < max_letter_frequency[idx])
            try_add(static_cast<char>('A' + idx), consume_gem());
    }
}

} // namespace

void Rack::removeTile(const Tile& tile)
{
    const auto it = std::find(mTiles.begin(), mTiles.end(), tile);
    if (it != mTiles.end())
        mTiles.erase(it);
}

void Rack::regenerateTiles(Gem gem, bool wildcard, std::mt19937& rng)
{
    auto& dist = distribution();
    regenerate_core(mTiles, mSize, gem, wildcard, [&] { return dist(rng); });
}

void Rack::regenerateTiles(Gem gem, bool wildcard, FastRng& rng)
{
    const auto& sampler = letter_sampler();
    regenerate_core(mTiles, mSize, gem, wildcard, [&] { return sampler(rng); });
}

void Rack::regenerateTilesCRN(Gem gem, bool wildcard, const int* draws)
{
    std::size_t i = 0;
    regenerate_core(mTiles, mSize, gem, wildcard, [&] { return draws[i++]; });
}

void Rack::dropGemOnRandomTile(Gem gem, std::mt19937& rng)
{
    if (gem == Gem::NONE)
        return;
    std::array<int, MAX_RACK_SIZE> eligible{};
    int n_eligible = 0;
    for (int i = 0; i < static_cast<int>(mTiles.size()); ++i)
    {
        if (!mTiles[i].isGem() && !mTiles[i].isWildcard())
            eligible[n_eligible++] = i;
    }
    if (n_eligible == 0)
        return;
    std::uniform_int_distribution<int> pick(0, n_eligible - 1);
    mTiles[eligible[pick(rng)]].setGem(gem);
}

void Rack::dropGemOnRandomTile(Gem gem, FastRng& rng)
{
    if (gem == Gem::NONE)
        return;
    std::array<int, MAX_RACK_SIZE> eligible{};
    int n_eligible = 0;
    for (int i = 0; i < static_cast<int>(mTiles.size()); ++i)
    {
        if (!mTiles[i].isGem() && !mTiles[i].isWildcard())
            eligible[n_eligible++] = i;
    }
    if (n_eligible == 0)
        return;
    mTiles[eligible[rng.below(n_eligible)]].setGem(gem);
}

void Rack::playTiles(const TileList& word_tiles)
{
    std::array<std::uint8_t, 26> demand_no_gem{};
    std::array<std::uint8_t, 26> demand_broken{};
    int demand_wild = 0;
    std::array<std::pair<std::int8_t, Gem>, MAX_WORD_LEN> gem_demands{};
    int n_gem_demands = 0;

    for (const Tile& t : word_tiles)
    {
        if (t.isWildcard())
            ++demand_wild;
        else if (t.isBroken())
            ++demand_broken[t.getLetter() - 'A'];
        else if (t.getGem() == Gem::NONE)
            ++demand_no_gem[t.getLetter() - 'A'];
        else
            gem_demands[n_gem_demands++] = {static_cast<std::int8_t>(t.getLetter() - 'A'), t.getGem()};
    }

    auto out = mTiles.begin();
    for (auto it = mTiles.begin(); it != mTiles.end(); ++it)
    {
        const Tile& t = *it;
        bool consumed = false;
        if (t.isWildcard())
        {
            if (demand_wild > 0)
            {
                --demand_wild;
                consumed = true;
            }
        }
        else
        {
            const int li = t.getLetter() - 'A';
            if (t.isBroken())
            {
                if (demand_broken[li] > 0)
                {
                    --demand_broken[li];
                    consumed = true;
                }
            }
            else if (t.getGem() == Gem::NONE)
            {
                if (demand_no_gem[li] > 0)
                {
                    --demand_no_gem[li];
                    consumed = true;
                }
            }
            else if (n_gem_demands > 0)
            {
                for (int k = 0; k < n_gem_demands; ++k)
                {
                    if (gem_demands[k].first == li && gem_demands[k].second == t.getGem())
                    {
                        gem_demands[k] = gem_demands[--n_gem_demands];
                        consumed = true;
                        break;
                    }
                }
            }
        }
        if (!consumed)
        {
            if (out != it)
                *out = t;
            ++out;
        }
    }
    mTiles.erase(out, mTiles.end());
}

void Rack::playWord(const Word& word)
{
    playTiles(word.getTiles());
}

void Rack::playWord(const Word& word, std::mt19937& rng)
{
    playWord(word);
    regenerateTiles(word.expectedGem(), word.checkWildcard(), rng);
}

void Rack::playWord(const Word& word, FastRng& rng)
{
    playWord(word);
    regenerateTiles(word.expectedGem(), word.checkWildcard(), rng);
}

ScoredHeap Rack::generateWordlist(
    const Trie<NUM_WORDS>& trie,
    int num_top_words,
    double power,
    bool powered
) const
{
    RackState state = build_rack_state(mTiles, power, config().gems_enabled, powered ? 1.25 : 1.0);
    return engine_find_top_words(trie, state, num_top_words);
}

std::vector<ScoredWord> Rack::generateKills(
    const Trie<NUM_WORDS>& trie,
    int threshold_damage,
    int max_candidates,
    double power,
    bool powered
) const
{
    RackState state = build_rack_state(mTiles, power, config().gems_enabled, powered ? 1.25 : 1.0);
    return engine_find_kill_words(trie, state, threshold_damage, max_candidates);
}

SearchResult Rack::bestWord(const Trie<NUM_WORDS>& trie, double power, bool powered) const
{
    RackState state = build_rack_state(mTiles, power, config().gems_enabled, powered ? 1.25 : 1.0);
    return engine_find_best_word(trie, state);
}

double Rack::incompleteRackScore(
    const Word& played_word,
    const Trie<NUM_WORDS>& trie,
    int num_top_words,
    int num_simulations,
    std::mt19937& rng,
    double power,
    bool powered
) const
{
    Rack residual = *this;
    residual.playWord(played_word);

    const Gem refill_gem = played_word.expectedGem();
    const bool refill_wildcard = played_word.checkWildcard();

    double total = 0.0;
    for (int sim = 0; sim < num_simulations; ++sim)
    {
        Rack draw(residual.getTiles(), residual.size());
        draw.regenerateTiles(refill_gem, refill_wildcard, rng);

        ScoredHeap top = draw.generateWordlist(trie, num_top_words, power, powered);
        if (top.empty())
            continue;

        double sum = 0.0;
        int cnt = 0;
        while (!top.empty())
        {
            sum += top.top().damage;
            top.pop();
            ++cnt;
        }
        total += sum / cnt;
    }
    return total / num_simulations;
}
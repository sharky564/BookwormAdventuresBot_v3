#pragma once
#include "constants.hpp"
#include "rack_search.hpp"
#include "rng.hpp"
#include "tile.hpp"
#include "trie.hpp"
#include "word.hpp"
#include <random>
#include <string>
#include <vector>

class Rack
{
public:
    explicit Rack(int size = MAX_RACK_SIZE) : mSize(size) {}

    explicit Rack(TileList tiles, int size = MAX_RACK_SIZE)
        :
        mTiles(std::move(tiles)),
        mSize(size)
    {}

    Rack(const std::string& letters, const std::vector<Gem>& gems, int size = MAX_RACK_SIZE)
        : mSize(size)
    {
        mTiles.reserve(letters.size());
        for (std::size_t i = 0; i < letters.size(); ++i)
            mTiles.emplace_back(letters[i], i < gems.size() ? gems[i] : Gem::NONE);
    }

    [[nodiscard]] const TileList& getTiles() const noexcept { return mTiles; }
    [[nodiscard]] int size() const noexcept { return mSize; }
    [[nodiscard]] int count() const noexcept { return static_cast<int>(mTiles.size()); }

    void addTile(const Tile& tile) { mTiles.push_back(tile); }
    void addTile(Tile&& tile) { mTiles.push_back(std::move(tile)); }
    void removeTile(const Tile& tile);

    void regenerateTiles(Gem gem, bool wildcard, std::mt19937& rng);
    void regenerateTiles(Gem gem, bool wildcard, FastRng& rng);
    void regenerateTilesCRN(Gem gem, bool wildcard, const int* draws);

    void dropGemOnRandomTile(Gem gem, std::mt19937& rng);
    void dropGemOnRandomTile(Gem gem, FastRng& rng);

    void playTiles(const TileList& word_tiles);
    void playWord(const Word& word);
    void playWord(const Word& word, std::mt19937& rng);
    void playWord(const Word& word, FastRng& rng);

    [[nodiscard]] ScoredHeap generateWordlist(
        const Trie<NUM_WORDS>& trie, int num_top_words, double power = 0.0, bool powered = false
    ) const;

    [[nodiscard]] std::vector<ScoredWord> generateKills(
        const Trie<NUM_WORDS>& trie, int threshold_damage,
        int max_candidates = 500, double power = 0.0, bool powered = false
    ) const;

    [[nodiscard]] SearchResult bestWord(const Trie<NUM_WORDS>& trie, double power = 0.0, bool powered = false) const;

    [[nodiscard]] double incompleteRackScore(
        const Word& played_word,
        const Trie<NUM_WORDS>& trie,
        int num_top_words,
        int num_simulations,
        std::mt19937& rng,
        double power = 0.0,
        bool powered = false
    ) const;

private:
    TileList mTiles;
    int mSize;
};
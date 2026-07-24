#pragma once
#include "constants.hpp"
#include "tile.hpp"
#include "utils.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

// Fixed-capacity inline tile sequence; racks and words never exceed
// MAX_RACK_SIZE tiles (protocol entry points validate this). Mirrors the
// std::vector<Tile> subset the codebase uses, without heap traffic.
class TileList
{
public:
    using value_type = Tile;
    using iterator = Tile*;
    using const_iterator = const Tile*;

    TileList() = default;

    [[nodiscard]] Tile* begin() noexcept { return mData.data(); }
    [[nodiscard]] const Tile* begin() const noexcept { return mData.data(); }
    [[nodiscard]] Tile* end() noexcept { return mData.data() + mSize; }
    [[nodiscard]] const Tile* end() const noexcept { return mData.data() + mSize; }
    [[nodiscard]] Tile* data() noexcept { return mData.data(); }
    [[nodiscard]] const Tile* data() const noexcept { return mData.data(); }

    [[nodiscard]] std::size_t size() const noexcept { return mSize; }
    [[nodiscard]] bool empty() const noexcept { return mSize == 0; }
    static constexpr std::size_t capacity() noexcept { return MAX_RACK_SIZE; }

    [[nodiscard]] Tile& operator[](std::size_t i) noexcept { return mData[i]; }
    [[nodiscard]] const Tile& operator[](std::size_t i) const noexcept { return mData[i]; }
    [[nodiscard]] Tile& front() noexcept { return mData[0]; }
    [[nodiscard]] const Tile& front() const noexcept { return mData[0]; }
    [[nodiscard]] Tile& back() noexcept { return mData[mSize - 1]; }
    [[nodiscard]] const Tile& back() const noexcept { return mData[mSize - 1]; }

    void push_back(const Tile& t) noexcept
    {
        assert(mSize < capacity());
        mData[mSize++] = t;
    }

    template <typename... Args>
    Tile& emplace_back(Args&&... args) noexcept
    {
        assert(mSize < capacity());
        mData[mSize] = Tile(std::forward<Args>(args)...);
        return mData[mSize++];
    }

    void pop_back() noexcept
    {
        assert(mSize > 0);
        --mSize;
    }

    void clear() noexcept { mSize = 0; }
    void reserve(std::size_t) noexcept {}

    Tile* erase(Tile* first, Tile* last) noexcept
    {
        std::move(last, end(), first);
        mSize = static_cast<std::uint8_t>(mSize - (last - first));
        return first;
    }
    Tile* erase(Tile* pos) noexcept { return erase(pos, pos + 1); }

    bool operator==(const TileList& other) const noexcept
    {
        return mSize == other.mSize && std::equal(begin(), end(), other.begin());
    }

private:
    std::array<Tile, MAX_RACK_SIZE> mData{};
    std::uint8_t mSize = 0;
};

static_assert(std::is_trivially_copyable_v<TileList>);

inline Point tile_points_sum(const TileList& tiles) noexcept
{
    Point sum = 0.0;
    for (const Tile& t : tiles)
        sum += t.getPoints();
    return sum;
}

// Refill metadata for a played word: the gem the refill spawns and whether it
// spawns a wildcard (rainbow, >= 3 distinct gem kinds). Shared by Word and
// the rollout cache so both compute identical values.
inline Gem expected_gem_for(const TileList& tiles) noexcept
{
    if (config().gems_enabled)
    {
        if (const int wp = static_cast<int>(std::ceil(tile_points_sum(tiles))); wp > 5)
            return gemMaxPoints(wp);
    }
    return Gem::NONE;
}

inline bool refill_wildcard_for(const TileList& tiles) noexcept
{
    if (!config().rainbow)
        return false;
    Gem first = Gem::NONE;
    Gem second = Gem::NONE;
    for (const auto& t : tiles)
    {
        if (!t.isGem())
            continue;
        const Gem g = t.getGem();
        if (first == Gem::NONE)
            first = g;
        else if (g != first)
        {
            if (second == Gem::NONE)
                second = g;
            else if (g != second)
                return true;
        }
    }
    return false;
}

class Word
{
public:
    Word() : mPoints(0.0) {}

    explicit Word(TileList tiles)
    :
    mTiles(tiles),
    mPoints(calculatePoints())
    {}

    [[nodiscard]] const TileList& getTiles() const noexcept { return mTiles; }
    [[nodiscard]] Point getPoints() const noexcept { return mPoints; }
    [[nodiscard]] bool empty() const noexcept { return mTiles.empty(); }

    [[nodiscard]] Point calculatePoints() const noexcept { return tile_points_sum(mTiles); }

    [[nodiscard]] int wordDamage(double power, bool gems_enabled, double boost = 1) const noexcept
    {
        double gem_sum = 0.0;
        if (gems_enabled)
        {
            for (const auto& t : mTiles)
                gem_sum += gemPower(t.getGem());
        }
        return compute_damage(mPoints, gem_sum, power, gems_enabled, boost);
    }

    [[nodiscard]] int wordDamage() const noexcept
    {
        return wordDamage(0.0, config().gems_enabled);
    }

    [[nodiscard]] Gem expectedGem() const noexcept
    {
        if (config().gems_enabled)
        {
            if (const int wp = static_cast<int>(std::ceil(mPoints)); wp > 5)
                return gemMaxPoints(wp);
        }
        return Gem::NONE;
    }

    [[nodiscard]] bool checkWildcard() const noexcept { return refill_wildcard_for(mTiles); }

    [[nodiscard]] std::string wordString() const
    {
        std::string s;
        s.reserve(mTiles.size());
        for (const auto& t : mTiles)
            s += t.getLetter();
        return s;
    }

    [[nodiscard]] std::string gemString() const
    {
        std::string s; s.reserve(mTiles.size());
        for (const auto& t : mTiles)
            s += gemCharacter(t.getGem());
        return s;
    }

    bool operator==(const Word& other) const noexcept { return mTiles == other.mTiles; }

    bool operator>(const Word& other) const noexcept
    {
        const int d1 = wordDamage();
        const int d2 = other.wordDamage();
        if (d1 != d2)
            return d1 > d2;
        if (mTiles.size() != other.mTiles.size())
            return mTiles.size() > other.mTiles.size();
        return wordString() > other.wordString();
    }

private:
    TileList mTiles;
    Point mPoints;
};

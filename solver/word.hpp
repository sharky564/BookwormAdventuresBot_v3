#pragma once
#include "constants.hpp"
#include "tile.hpp"
#include "utils.hpp"
#include <numeric>
#include <optional>
#include <string>
#include <vector>

using TileList = std::vector<Tile>;

class Word
{
public:
    Word() : mPoints(0.0) {}

    explicit Word(TileList tiles)
    :
    mTiles(std::move(tiles)),
    mPoints(calculatePoints())
    {}

    [[nodiscard]] const TileList& getTiles() const noexcept { return mTiles; }
    [[nodiscard]] Point getPoints() const noexcept { return mPoints; }
    [[nodiscard]] bool empty() const noexcept { return mTiles.empty(); }

    [[nodiscard]] Point calculatePoints() const noexcept
    {
        return std::accumulate(
            mTiles.begin(), mTiles.end(), 0.0, [](Point sum, const Tile& t) { return sum + t.getPoints(); }
        );
    }

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

    [[nodiscard]] bool checkWildcard() const noexcept
    {
        if (!config().rainbow)
            return false;
        Gem first = Gem::NONE;
        Gem second = Gem::NONE;
        for (const auto& t : mTiles)
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
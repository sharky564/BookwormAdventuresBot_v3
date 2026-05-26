#pragma once
#include "constants.hpp"
#include "utils.hpp"

class Tile
{
public:
    explicit Tile(char letter, Gem gem = Gem::NONE) noexcept
    :
    mPoints(letter == WILDCARD_CHAR ? WILDCARD_PTS : config().letter_points[static_cast<unsigned>(letter - 'A')]),
    mGem(gem),
    mLetter(letter)
    {}

    [[nodiscard]] char getLetter() const noexcept { return mLetter; }
    [[nodiscard]] Gem getGem() const noexcept { return mGem; }
    [[nodiscard]] Point getPoints() const noexcept { return mPoints; }
    [[nodiscard]] bool isGem() const noexcept { return mGem != Gem::NONE; }
    [[nodiscard]] bool isWildcard() const noexcept { return mLetter == WILDCARD_CHAR; }

    bool operator==(const Tile& other) const noexcept { return mLetter == other.mLetter && mGem == other.mGem; }

private:
    Point mPoints;
    Gem mGem;
    char mLetter;
};
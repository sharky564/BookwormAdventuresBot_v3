#pragma once
#include "constants.hpp"
#include "utils.hpp"

// 2 bytes, trivially copyable; points come from the live config so a Tile
// never holds state a letter_points reconfiguration could stale.
class Tile
{
public:
    constexpr Tile() noexcept = default;

    explicit Tile(char letter, Gem gem = Gem::NONE) noexcept
    :
    mGem(gem),
    mLetter(letter)
    {}

    [[nodiscard]] char getLetter() const noexcept { return mLetter; }
    [[nodiscard]] Gem getGem() const noexcept { return mGem; }
    [[nodiscard]] Point getPoints() const noexcept
    {
        return mLetter == WILDCARD_CHAR
            ? WILDCARD_PTS
            : config().letter_points[static_cast<unsigned>(mLetter - 'A')];
    }
    [[nodiscard]] bool isGem() const noexcept { return mGem != Gem::NONE; }
    [[nodiscard]] bool isWildcard() const noexcept { return mLetter == WILDCARD_CHAR; }

    void setGem(Gem gem) noexcept { mGem = gem; }

    bool operator==(const Tile& other) const noexcept { return mLetter == other.mLetter && mGem == other.mGem; }

private:
    Gem mGem = Gem::NONE;
    char mLetter = 0;
};

static_assert(sizeof(Tile) == 2, "Tile should stay 2 bytes");

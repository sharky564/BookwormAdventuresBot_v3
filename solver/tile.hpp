#pragma once
#include "constants.hpp"
#include "utils.hpp"

// 3 bytes, trivially copyable; points come from the live config so a Tile
// never holds state a letter_points reconfiguration could stale. Broken
// tiles (smashed/plagued in-game) spell normally but score 0 points and
// carry no gem.
class Tile
{
public:
    constexpr Tile() noexcept = default;

    explicit Tile(char letter, Gem gem = Gem::NONE, bool broken = false) noexcept
    :
    mGem(broken ? Gem::NONE : gem),
    mLetter(letter),
    mBroken(broken)
    {}

    [[nodiscard]] char getLetter() const noexcept { return mLetter; }
    [[nodiscard]] Gem getGem() const noexcept { return mGem; }
    [[nodiscard]] Point getPoints() const noexcept
    {
        if (mBroken)
            return 0.0;
        return mLetter == WILDCARD_CHAR
            ? WILDCARD_PTS
            : config().letter_points[static_cast<unsigned>(mLetter - 'A')];
    }
    [[nodiscard]] bool isGem() const noexcept { return mGem != Gem::NONE; }
    [[nodiscard]] bool isWildcard() const noexcept { return mLetter == WILDCARD_CHAR; }
    [[nodiscard]] bool isBroken() const noexcept { return mBroken; }

    void setGem(Gem gem) noexcept
    {
        if (!mBroken)
            mGem = gem;
    }

    bool operator==(const Tile& other) const noexcept
    {
        return mLetter == other.mLetter && mGem == other.mGem && mBroken == other.mBroken;
    }

private:
    Gem mGem = Gem::NONE;
    char mLetter = 0;
    bool mBroken = false;
};

static_assert(sizeof(Tile) == 3, "Tile should stay 3 bytes");

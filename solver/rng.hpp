#pragma once
//==============================================================================
// rng.hpp -- fast RNG for the Monte-Carlo hot paths: FastRng (xoshiro256++)
// plus LetterSampler (Walker alias method over the exact tile weights), one
// 64-bit draw per tile. handle_step deliberately keeps mt19937 + the old
// distribution -- its streams are protocol-seeded and Python-visible.
//==============================================================================
#include "constants.hpp"
#include <array>
#include <cstdint>

class FastRng
{
public:
    using result_type = std::uint64_t;
    static constexpr result_type min() noexcept { return 0; }
    static constexpr result_type max() noexcept { return ~result_type{0}; }

    explicit FastRng(std::uint64_t seed_value = 0xF00DF00DF00DF00Dull) noexcept { seed(seed_value); }

    void seed(std::uint64_t seed_value) noexcept
    {
        // splitmix64 expansion of the seed into the 256-bit state.
        std::uint64_t x = seed_value;
        for (auto& si : mState)
        {
            x += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            si = z ^ (z >> 31);
        }
    }

    result_type operator()() noexcept
    {
        const std::uint64_t result = rotl(mState[0] + mState[3], 23) + mState[0];
        const std::uint64_t t = mState[1] << 17;
        mState[2] ^= mState[0];
        mState[3] ^= mState[1];
        mState[1] ^= mState[2];
        mState[0] ^= mState[3];
        mState[2] ^= t;
        mState[3] = rotl(mState[3], 45);
        return result;
    }

    // Uniform integer in [0, n) via Lemire multiply-shift on the high bits.
    int below(int n) noexcept
    {
        const std::uint32_t r = static_cast<std::uint32_t>((*this)() >> 32);
        return static_cast<int>((static_cast<std::uint64_t>(r) * static_cast<std::uint32_t>(n)) >> 32);
    }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept
    {
        return (x << k) | (x >> (64 - k));
    }

    std::array<std::uint64_t, 4> mState{};
};

// Alias sampler over the 26-letter tile distribution, built from the same
// integer weights as distribution() (sum 97,380) in 32-bit fixed point.
class LetterSampler
{
public:
    LetterSampler() noexcept
    {
        constexpr std::array<std::uint32_t, 26> weights = {
            9420, 2200, 2000, 4200, 12700, 2000, 2500, 2000, 7330, 700,
            700, 4000, 2300, 6000, 7330, 2300, 600, 6900, 5600, 6000,
            4400, 1600, 1500, 500, 2100, 500,
        };
        std::uint64_t total = 0;
        for (const auto w : weights)
            total += w;

        // Scaled weights: w * 26 * 2^32 / total, in 64-bit fixed point.
        std::array<std::uint64_t, 26> scaled{};
        for (int i = 0; i < 26; ++i)
            scaled[i] = (static_cast<std::uint64_t>(weights[i]) * 26u << 32) / total;

        constexpr std::uint64_t ONE = std::uint64_t{1} << 32;
        std::array<int, 26> small{}, large{};
        int ns = 0, nl = 0;
        for (int i = 0; i < 26; ++i)
            (scaled[i] < ONE ? small[ns++] : large[nl++]) = i;

        while (ns > 0 && nl > 0)
        {
            const int s = small[--ns];
            const int l = large[--nl];
            mProb[s] = static_cast<std::uint32_t>(scaled[s]);
            mAlias[s] = static_cast<std::uint8_t>(l);
            scaled[l] -= (ONE - scaled[s]);
            (scaled[l] < ONE ? small[ns++] : large[nl++]) = l;
        }
        while (nl > 0)
        {
            const int l = large[--nl];
            mProb[l] = 0xFFFFFFFFu;
            mAlias[l] = static_cast<std::uint8_t>(l);
        }
        while (ns > 0)  // numerical residue; probability ~1 by construction
        {
            const int s = small[--ns];
            mProb[s] = 0xFFFFFFFFu;
            mAlias[s] = static_cast<std::uint8_t>(s);
        }
    }

    int operator()(FastRng& rng) const noexcept
    {
        const std::uint64_t r = rng();
        const int i = static_cast<int>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(r >> 32)) * 26u) >> 32);
        return static_cast<std::uint32_t>(r) < mProb[i] ? i : mAlias[i];
    }

private:
    std::array<std::uint32_t, 26> mProb{};
    std::array<std::uint8_t, 26> mAlias{};
};

inline const LetterSampler& letter_sampler()
{
    static const LetterSampler s;
    return s;
}

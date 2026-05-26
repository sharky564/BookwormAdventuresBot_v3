#pragma once
#include "constants.hpp"
#include <algorithm>
#include <cmath>

namespace Utils {

constexpr int round_half_up(double x) noexcept
{
    return static_cast<int>(x + 0.5);
}

} // namespace Utils

constexpr Gem gemMaxPoints(int points) noexcept
{
    switch (points)
    {
    case 6:  return Gem::AMETHYST;
    case 7:  return Gem::EMERALD;
    case 8:  return Gem::SAPPHIRE;
    case 9:  return Gem::GARNET;
    case 10: return Gem::SAPPHIRE;
    case 11: return Gem::CRYSTAL;
    case 12:
    case 13:
    case 14:
    case 15:
    case 16: return Gem::DIAMOND;
    default: return Gem::NONE;
    }
}

constexpr double gemPower(Gem gem) noexcept
{
    switch (gem)
    {
    case Gem::NONE:
        return 0.0;
    case Gem::AMETHYST:
        return 0.15;
    case Gem::EMERALD:
        return 0.20;
    case Gem::SAPPHIRE:
        return 0.25;
    case Gem::GARNET:
        return 0.30;
    case Gem::RUBY:
        return 0.35;
    case Gem::CRYSTAL:
        return 0.50;
    case Gem::DIAMOND:
        return 1.00;
    }
    return 0.0;
}

constexpr Gem translateGem(char gem) noexcept
{
    switch (gem)
    {
    case 'a':
        return Gem::AMETHYST;
    case 'e':
        return Gem::EMERALD;
    case 's':
        return Gem::SAPPHIRE;
    case 'g':
        return Gem::GARNET;
    case 'r':
        return Gem::RUBY;
    case 'c':
        return Gem::CRYSTAL;
    case 'd':
        return Gem::DIAMOND;
    default:
        return Gem::NONE;
    }
}

constexpr char gemCharacter(Gem gem) noexcept
{
    switch (gem)
    {
    case Gem::NONE:
        return ' ';
    case Gem::AMETHYST:
        return 'a';
    case Gem::EMERALD:
        return 'e';
    case Gem::SAPPHIRE:
        return 's';
    case Gem::GARNET:
        return 'g';
    case Gem::RUBY:
        return 'r';
    case Gem::CRYSTAL:
        return 'c';
    case Gem::DIAMOND:
        return 'd';
    }
    return ' ';
}

inline int compute_damage(
    double raw_points,
    double gem_power_sum,
    double power,
    bool gems_enabled,
    double power_boost = 1.0,
    double treasure_boost = 1.0,
    double weakness_boost = 1.0,
    int base_damage_bonus = 0,
    int armour = 0
) noexcept
{
    const int clamped = std::clamp(Utils::round_half_up(raw_points), 2, 16);
    const double Q = static_cast<double>(quarterHeartsTable[clamped - 2]);

    double inner = Q * (1.0 + power * 0.01);
    if (gems_enabled)
        inner += std::ceil(gem_power_sum * Q);
    const double boosted = treasure_boost * power_boost * inner;
    const double pre_floor = weakness_boost * (boosted + static_cast<double>(base_damage_bonus));
    int total = static_cast<int>(std::floor(pre_floor)) - armour;
    if (total < 0)
        total = 0;
    return total;
}
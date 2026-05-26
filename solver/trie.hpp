#pragma once
#include "constants.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class TrieLoadError { FileNotFound, ReadError };

struct TrieNode
{
    std::uint32_t bitmap;
    std::uint32_t children_offset;
    std::int8_t max_depth;
    std::uint8_t finish_mask;
    std::uint8_t subtree_bonuses;
    std::uint8_t _pad;
};
static_assert(sizeof(TrieNode) == 12, "TrieNode must be 12 bytes");

inline constexpr std::uint8_t WORD_FINISHED_BIT = 1u << 0;
inline constexpr int NUM_BONUS_CATS = 5;
inline constexpr std::uint8_t bonus_bit_for(int cat) { return static_cast<std::uint8_t>(1u << (cat + 1)); }
inline constexpr std::uint8_t ALL_BONUS_BITS = static_cast<std::uint8_t>(0b00111110u);

template <int size>
class Trie
{
public:
    static constexpr int ROOT = 0;
    static constexpr std::uint32_t BITMAP_LETTERS = 0x03FF'FFFFu;

    std::vector<TrieNode> nodes;
    std::vector<std::int32_t> children;
    std::vector<std::uint8_t> child_letters;

    Trie() = default;
    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;

    void add_word(std::string_view word);
    [[nodiscard]] bool is_word(std::string_view word) const noexcept;

    [[nodiscard]] constexpr static int get_root() noexcept { return ROOT; }
    [[nodiscard]] int node_count() const noexcept { return mFinalized ? static_cast<int>(nodes.size()) : mNext; }
    [[nodiscard]] int edge_count() const noexcept { return static_cast<int>(children.size()); }
    [[nodiscard]] bool finalized()  const noexcept { return mFinalized; }

    [[nodiscard]] std::expected<int, TrieLoadError> load(const std::string& filename);

    [[nodiscard]] std::expected<int, TrieLoadError> load_bonus(
        const std::string& filename, int cat_idx, int* mismatches = nullptr);

    void finalize();

    void reset();

    [[nodiscard]] int child_for_letter(int node, int letter) const noexcept
    {
        const auto& n = nodes[node];
        const std::uint32_t mask = std::uint32_t{1} << letter;
        if (!(n.bitmap & mask))
            return -1;
        const std::uint32_t count = std::popcount(n.bitmap);
        const std::uint32_t base = n.children_offset;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (child_letters[base + i] == static_cast<std::uint8_t>(letter))
                return children[base + i];
        }
        return -1;
    }

private:
    std::unique_ptr<std::int32_t[]> mBuild;
    std::unique_ptr<std::bitset<size>> mBuildWordFinished;
    std::unique_ptr<std::uint8_t[]> mBuildBonusBits;
    int mNext = 1;
    bool mFinalized = false;

    void ensure_build();
    void ensure_bonus_build();
    std::int8_t compute_max_depth_build(int node, std::vector<std::int8_t>& out) const;
    std::uint8_t compute_subtree_bonuses(int node);
};

template <int size>
void Trie<size>::ensure_build()
{
    if (mBuild)
        return;
    const std::size_t flat = static_cast<std::size_t>(size) * 26;
    mBuild.reset(new std::int32_t[flat]);
    std::fill_n(mBuild.get(), flat, -1);
    mBuildWordFinished = std::make_unique<std::bitset<size>>();
}

template <int size>
void Trie<size>::ensure_bonus_build()
{
    if (mBuildBonusBits)
        return;
    mBuildBonusBits.reset(new std::uint8_t[size]());
}

template <int size>
void Trie<size>::add_word(std::string_view word)
{
    if (mFinalized)
        return;
    ensure_build();
    int curr = ROOT;
    for (char c : word)
    {
        const int idx = c - 'A';
        const int flat = curr * 26 + idx;
        if (mBuild[flat] == -1)
            mBuild[flat] = mNext++;
        curr = mBuild[flat];
    }
    (*mBuildWordFinished)[curr] = true;
}

template <int size>
bool Trie<size>::is_word(std::string_view word) const noexcept
{
    if (mFinalized)
    {
        int curr = ROOT;
        for (char c : word)
        {
            curr = child_for_letter(curr, c - 'A');
            if (curr == -1)
                return false;
        }
        return (nodes[curr].finish_mask & WORD_FINISHED_BIT) != 0;
    }
    if (!mBuild)
        return false;
    int curr = ROOT;
    for (char c : word)
    {
        curr = mBuild[curr * 26 + (c - 'A')];
        if (curr == -1)
            return false;
    }
    return mBuildWordFinished && (*mBuildWordFinished)[curr];
}

template <int size>
std::expected<int, TrieLoadError> Trie<size>::load(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
        return std::unexpected(TrieLoadError::FileNotFound);
    std::string word;
    int loaded = 0;
    while (file >> word)
    {
        add_word(word);
        ++loaded;
    }
    if (file.bad())
        return std::unexpected(TrieLoadError::ReadError);
    return loaded;
}

template <int size>
std::expected<int, TrieLoadError> Trie<size>::load_bonus(const std::string& filename, int cat_idx, int* mismatches)
{
    if (mFinalized)
        return std::unexpected(TrieLoadError::ReadError);
    if (cat_idx < 0 || cat_idx >= NUM_BONUS_CATS)
        return std::unexpected(TrieLoadError::ReadError);
    if (!mBuild || !mBuildWordFinished)
        return std::unexpected(TrieLoadError::ReadError);
    std::ifstream file(filename);
    if (!file.is_open())
        return std::unexpected(TrieLoadError::FileNotFound);

    ensure_bonus_build();
    const std::uint8_t bit = bonus_bit_for(cat_idx);

    std::string word;
    int matched = 0;
    int mismatch_count = 0;
    while (file >> word)
    {
        int curr = ROOT;
        bool path_ok = true;
        for (char c : word)
        {
            const int idx = c - 'A';
            if (idx < 0 || idx >= 26)
            {
                path_ok = false;
                break;
            }
            const int next = mBuild[curr * 26 + idx];
            if (next == -1)
            {
                path_ok = false;
                break;
            }
            curr = next;
        }
        if (!path_ok || !(*mBuildWordFinished)[curr])
        {
            ++mismatch_count;
            continue;
        }
        mBuildBonusBits[curr] |= bit;
        ++matched;
    }
    if (file.bad())
        return std::unexpected(TrieLoadError::ReadError);
    if (mismatches)
        *mismatches = mismatch_count;
    return matched;
}

template <int size>
std::int8_t Trie<size>::compute_max_depth_build(int node, std::vector<std::int8_t>& out) const
{
    int best = (*mBuildWordFinished)[node] ? 0 : -1;
    const int base = node * 26;
    for (int i = 0; i < 26; ++i)
    {
        const int child = mBuild[base + i];
        if (child == -1)
            continue;
        const int d = compute_max_depth_build(child, out);
        if (d >= 0 && d + 1 > best)
            best = d + 1;
    }
    out[node] = static_cast<std::int8_t>(best);
    return out[node];
}

template <int size>
void Trie<size>::finalize()
{
    if (mFinalized)
        return;
    if (!mBuild)
    {
        mFinalized = true;
        return;
    }

    const int n = mNext;

    std::vector<std::int8_t> depths(n);
    compute_max_depth_build(ROOT, depths);

    nodes.resize(n);
    std::uint32_t total_edges = 0;
    for (int i = 0; i < n; ++i)
    {
        std::uint32_t bm = 0;
        const int base = i * 26;
        for (int j = 0; j < 26; ++j)
        {
            if (mBuild[base + j] != -1)
                bm |= (std::uint32_t{1} << j);
        }
        nodes[i].bitmap = bm;
        nodes[i].max_depth = depths[i];
        std::uint8_t fm = (*mBuildWordFinished)[i] ? WORD_FINISHED_BIT : 0;
        if (mBuildBonusBits)
            fm |= mBuildBonusBits[i];
        nodes[i].finish_mask = fm;
        nodes[i].subtree_bonuses = 0;
        nodes[i]._pad = 0;
        total_edges += std::popcount(bm);
    }

    std::uint32_t offset = 0;
    for (int i = 0; i < n; ++i)
    {
        nodes[i].children_offset = offset;
        offset += std::popcount(nodes[i].bitmap);
    }

    children.resize(total_edges);
    child_letters.resize(total_edges);
    for (int i = 0; i < n; ++i)
    {
        std::uint32_t out_idx = nodes[i].children_offset;
        const int base = i * 26;
        for (int j = 0; j < 26; ++j)
        {
            if (mBuild[base + j] != -1)
            {
                children[out_idx] = mBuild[base + j];
                child_letters[out_idx] = static_cast<std::uint8_t>(j);
                ++out_idx;
            }
        }
    }

    compute_subtree_bonuses(ROOT);

    struct Entry
    {
        std::int8_t depth;
        std::int32_t child;
        std::uint8_t letter;
    };
    std::array<Entry, 26> buf{};
    for (int i = 0; i < n; ++i)
    {
        const std::uint32_t count = std::popcount(nodes[i].bitmap);
        if (count <= 1)
            continue;
        const std::uint32_t base = nodes[i].children_offset;
        for (std::uint32_t k = 0; k < count; ++k)
        {
            const std::int32_t c = children[base + k];
            buf[k] = {nodes[c].max_depth, c, child_letters[base + k]};
        }
        std::sort(buf.begin(), buf.begin() + count, [](const Entry& a, const Entry& b) { return a.depth > b.depth; });
        for (std::uint32_t k = 0; k < count; ++k)
        {
            children[base + k] = buf[k].child;
            child_letters[base + k] = buf[k].letter;
        }
    }

    mBuild.reset();
    mBuildWordFinished.reset();
    mBuildBonusBits.reset();
    mFinalized = true;
}

template <int size>
std::uint8_t Trie<size>::compute_subtree_bonuses(int node)
{
    std::uint8_t mask = nodes[node].finish_mask & ALL_BONUS_BITS;
    const std::uint32_t bits = nodes[node].bitmap;
    const std::uint32_t base = nodes[node].children_offset;
    int slot = 0;
    for (std::uint32_t b = bits; b; b &= b - 1, ++slot)
    {
        const int child = children[base + slot];
        mask |= compute_subtree_bonuses(child);
    }
    nodes[node].subtree_bonuses = mask;
    return mask;
}

template <int size>
void Trie<size>::reset()
{
    nodes.clear();
    children.clear();
    mBuild.reset();
    mBuildWordFinished.reset();
    mBuildBonusBits.reset();
    mNext = 1;
    mFinalized = false;
}
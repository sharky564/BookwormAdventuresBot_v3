"""Pure decision logic for the TAS.

Extracted from main.py so it can be unit-tested without Tk, screen capture,
or a running engine (tests/test_bot_logic.py). Everything here is a pure
function of its inputs; UI, threading, and engine I/O stay in main.py.
"""
from __future__ import annotations

from collections.abc import Iterable, Sequence

from engine import Tile

# Tile-state semantics (game rules, not vision classes): smashed and plagued
# tiles still spell words but score 0 damage; locked tiles are unplayable.
BROKEN_STATES = ("smashed", "plague")


def letter_multiset(s: str) -> dict[str, int]:
    d: dict[str, int] = {}
    for ch in s:
        d[ch] = d.get(ch, 0) + 1
    return d


def can_form(word: str, available: dict[str, int]) -> bool:
    need = letter_multiset(word)
    return all(available.get(ch, 0) >= k for ch, k in need.items())


def conservation_penalty(word_tiles: Sequence[Tile], upcoming_words: Iterable[str]) -> int:
    """How badly playing these tiles eats into letters needed by upcoming
    scripted answers (max per-letter demand across all upcoming words)."""
    demand: dict[str, int] = {}
    for w in upcoming_words:
        for ch, k in letter_multiset(w).items():
            demand[ch] = max(demand.get(ch, 0), k)
    if not demand:
        return 0
    consumed = letter_multiset("".join(t.letter for t in word_tiles))
    return sum(min(k, demand[ch]) for ch, k in consumed.items() if ch in demand)


def choose_sphinx_variant(
    answers: Sequence[str], rack_letters: str, upcoming: Sequence[str]
) -> str | None:
    """Pick the formable sphinx answer variant that consumes the fewest
    letters needed by upcoming answers; ties go to the shorter word."""
    available = letter_multiset(rack_letters)
    formable = [w for w in answers if can_form(w, available)]
    if not formable:
        return None

    def cost(w: str) -> tuple[int, int]:
        tiles = [Tile(letter=ch) for ch in w]
        return (conservation_penalty(tiles, upcoming), len(w))

    return min(formable, key=cost)


def partition_playable(recs: Sequence) -> tuple[list, list]:
    """Split recognitions into (playable, excluded). Smashed/plagued tiles
    with readable letters count as playable (0-damage); locked tiles and
    broken tiles with unreadable letters do not."""
    playable: list = []
    excluded: list = []
    for r in recs:
        if r.status == "normal" or (r.status in BROKEN_STATES and r.letter):
            playable.append(r)
        else:
            excluded.append(r)
    return playable, excluded


def broken_mask(playable: Sequence) -> str:
    """Solver-protocol broken mask for a playable-recognition list; empty
    string when no tile is broken."""
    mask = "".join("X" if r.status in BROKEN_STATES else "." for r in playable)
    return mask if "X" in mask else ""

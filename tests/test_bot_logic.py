"""Unit tests for bot_logic. Runnable directly (no pytest needed):

    python tests/test_bot_logic.py

or, if pytest is available: pytest tests/
"""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from bot_logic import (
    broken_mask,
    can_form,
    choose_sphinx_variant,
    conservation_penalty,
    letter_multiset,
    partition_playable,
)
from engine import Tile


@dataclass
class Rec:  # minimal Recognition stand-in
    letter: str
    status: str = "normal"
    gem: str = "none"


def tiles(word: str) -> list[Tile]:
    return [Tile(letter=ch) for ch in word]


def test_letter_multiset():
    assert letter_multiset("") == {}
    assert letter_multiset("ABBA") == {"A": 2, "B": 2}


def test_can_form():
    avail = letter_multiset("ARSTEOIN")
    assert can_form("NOTARIES", avail)
    assert can_form("", avail)
    assert not can_form("SEES", avail)  # needs 2 S / 2 E
    assert not can_form("QUIZ", avail)


def test_conservation_penalty():
    assert conservation_penalty(tiles("SKY"), []) == 0
    # Upcoming WALL needs W,A,L,L; playing SKY touches none of them.
    assert conservation_penalty(tiles("SKY"), ["WALL"]) == 0
    # Playing WALLS consumes W,A,L,L (S unneeded): penalty 4.
    assert conservation_penalty(tiles("WALLS"), ["WALL"]) == 4
    # Demand is the max per letter across upcoming, not the sum.
    assert conservation_penalty(tiles("LL"), ["WALL", "WELL"]) == 2


def test_choose_sphinx_variant():
    # Both formable; SKY consumes fewer letters needed later and is shorter.
    assert choose_sphinx_variant(["SKY", "SKIES"], "SKYIESXX", ["FISTS"]) == "SKY"
    # Only the longer variant formable.
    assert choose_sphinx_variant(["SKY", "SKIES"], "SKIEBBBS", []) == "SKIES"
    # Neither formable.
    assert choose_sphinx_variant(["SKY", "SKIES"], "BBBBBBBB", []) is None
    # Conservation beats length: TRUTHS spares the S needed by... no -- use a
    # case where the shorter variant conflicts with upcoming and longer wins.
    assert (
        choose_sphinx_variant(["FIST", "FISTS"], "FISTSFIST", ["FIST"]) == "FIST"
    )


def test_partition_playable():
    recs = [
        Rec("A"),
        Rec("B", status="smashed"),
        Rec("C", status="plague"),
        Rec("D", status="locked"),
        Rec("", status="smashed"),  # unreadable broken tile -> excluded
    ]
    playable, excluded = partition_playable(recs)
    assert [r.letter for r in playable] == ["A", "B", "C"]
    assert [r.status for r in excluded] == ["locked", "smashed"]


def test_broken_mask():
    playable, _ = partition_playable(
        [Rec("A"), Rec("B", status="smashed"), Rec("C", status="plague")]
    )
    assert broken_mask(playable) == ".XX"
    assert broken_mask([Rec("A"), Rec("B")]) == ""


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS {name}")
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
    sys.exit(1 if failures else 0)

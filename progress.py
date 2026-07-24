from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
STATE_PATH = _SCRIPT_DIR / "wordgame_state.json"


POWER_TABLE: list[list[list[float]]] = [
    [
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0],  # 1.1
        [0.0, 0.0, 0.0, 0.0, 6.3, 6.3],  # 1.2
        [9.0, 9.5, 10.6, 12.2, 13.9, 17.1, 29.6],  # 1.3
        [31.8, 32.5, 33.2, 34.3, 35.7, 37.4, 39.6],  # 1.4
        [39.6, 40.9, 42.4, 44.3, 46.5, 49.0, 49.0],  # 1.5
        [51.0, 52.8, 54.8, 57.2, 59.7, 72.2],  # 1.6
        [74.7, 74.7, 74.7, 74.7, 74.7, 74.7, 74.7],  # 1.7
        [74.7, 76.9, 79.2, 81.6, 84.1, 84.1],  # 1.8
        [85.9, 87.49, 89.1, 90.8, 92.6, 94.4, 106.9],  # 1.9
        [109.1, 110.9, 112.8, 114.7, 116.6, 118.6],  # 1.10
    ],
    [
        [118.6, 119.6, 120.9, 122.7, 124.8, 127.7],  # 2.1
        [127.7, 128.9, 130.4, 132.4, 134.6, 137.1],  # 2.2
        [149.6, 151.5, 153.8, 156.3, 159.0],  # 2.3
        [159.3],  # 2.4
        [159.3, 159.3, 161.4, 163.7, 166.3],  # 2.5
        [169.8, 182.3, 184.6, 187.1, 189.7],  # 2.6
        [192.6, 192.6, 194.9, 197.4, 199.9],  # 2.7
        [203.3, 203.3, 205.4, 207.9, 210.8],  # 2.8
        [213.9, 226.4, 228.6, 231.0, 233.8],  # 2.9
        [237.0, 237.0, 238.7, 241.1, 243.8],  # 2.10
    ],
    [
        [247.2, 247.2, 249.2, 251.6, 254.5],  # 3.1
        [258.1, 270.6, 272.7, 275.2, 278.0],  # 3.2
        [281.4, 281.4, 283.4, 285.8, 288.7],  # 3.3
        [292.2, 292.2, 294.3, 296.7, 299.6],  # 3.4
        [302.9, 315.4, 317.5, 319.9, 322.7],  # 3.5
        [326.0, 326.0, 328.1, 326.0, 333.4],  # 3.6
        [336.7, 336.7, 339.5, 343.0],  # 3.7
        [347.1, 359.6, 361.5, 364.0, 366.9],  # 3.8
        [370.6, 370.6, 372.3, 374.6, 377.6],  # 3.9
        [381.7, 381.7, 381.7, 381.7, 381.7],  # 3.10
    ],
]


MONSTER_TABLE: list[list[list[tuple[int, int]]]] = [
    [
        [(1, 0), (2, 0), (3, 0), (3, 0), (2, 0), (3, 0)],  # 1.1
        [(3, 0), (2, 0), (4, 0), (3, 0), (5, 0), (7, 0)],  # 1.2
        [(6, 0), (5, 0), (5, 0), (6, 0), (6, 0), (7, 0), (8, 0)],  # 1.3
        [(4, 0), (4, 0), (6, 0), (6, 0), (7, 0), (7, 0), (10, 0)],  # 1.4
        [(7, 0), (7, 0), (8, 0), (7, 0), (8, 0), (10, 0), (12, 0)],  # 1.5
        [(7, 0), (6, 0), (8, 0), (9, 0), (8, 0), (16, 0)],  # 1.6
        [(4, 0), (4, 0), (4, 0), (4, 0), (5, 0), (5, 0), (7, 0)],  # 1.7
        [(10, 0), (12, 0), (14, 0), (15, 0), (16, 0), (20, 0)],  # 1.8
        [(17, 0), (18, 0), (18, 0), (18, 0), (19, 0), (20, 0), (25, 0)],  # 1.9
        [(25, 0), (24, 0), (25, 0), (24, 0), (28, 0), (30, 0)],  # 1.10
    ],
    [
        [(15, 0), (16, 0), (14, 0), (15, 0), (17, 0), (25, 0)],  # 2.1
        [(20, 0), (22, 0), (22, 0), (22, 0), (22, 0), (24, 0)],  # 2.2
        [(20, 0), (20, 0), (20, 0), (24, 0), (30, 0)],  # 2.3
        [(9, 0)],  # 2.4
        [(20, 0), (24, 0), (22, 0), (24, 0), (30, 0)],  # 2.5
        [(20, 0), (20, 0), (22, 0), (25, 0), (30, 0)],  # 2.6
        [(22, 0), (20, 0), (24, 0), (25, 0), (30, 0)],  # 2.7
        [(24, 0), (24, 0), (26, 0), (26, 0), (30, 0)],  # 2.8
        [(16, 6), (14, 6), (20, 6), (18, 6), (20, 6)],  # 2.9
        [(24, 6), (22, 6), (22, 6), (20, 6), (30, 8)],  # 2.10
    ],
    [
        [(18, 6), (20, 6), (25, 6), (24, 6), (30, 6)],  # 3.1
        [(22, 6), (24, 6), (24, 6), (26, 6), (30, 6)],  # 3.2
        [(8, 12), (12, 12), (15, 12), (17, 12), (20, 12)],  # 3.3
        [(17, 12), (18, 12), (22, 12), (22, 12), (25, 12)],  # 3.4
        [(18, 12), (19, 12), (20, 12), (20, 12), (24, 12)],  # 3.5
        [(16, 12), (18, 12), (15, 12), (14, 12), (24, 12)],  # 3.6
        [(25, 13), (25, 13), (20, 12), (25, 13)],  # 3.7
        [(20, 12), (20, 12), (24, 12), (24, 12), (26, 13)],  # 3.8
        [(23, 12), (24, 12), (25, 12), (26, 12), (30, 12)],  # 3.9
        [(10, 7), (10, 8), (10, 9), (10, 10), (30, 12)],  # 3.10
    ],
]


_PRESET_TRANSITIONS: list[tuple[int, int, str]] = [
    (0, 0, "1.1"),
    (0, 1, "1.2"),
    (0, 3, "1.4"),
    (0, 5, "1.6"),
    (0, 7, "1.8"),
    (1, 5, "2.6"),
]


@dataclass
class Progress:
    book: int = 0
    chapter: int = 0
    enemy: int = 0

    def display(self) -> str:
        return f"{self.book + 1}.{self.chapter + 1}.{self.enemy + 1}"


def load_progress(path: Path = STATE_PATH) -> Progress:
    if not path.exists():
        return Progress()
    try:
        d = json.loads(path.read_text())
        return Progress(
            book=int(d["book"]), chapter=int(d["chapter"]), enemy=int(d["enemy"])
        )
    except Exception:
        return Progress()


def save_progress(p: Progress, path: Path = STATE_PATH) -> None:
    path.write_text(json.dumps(asdict(p), indent=2))


def power_at(p: Progress) -> float:
    try:
        return float(POWER_TABLE[p.book][p.chapter][p.enemy])
    except (IndexError, TypeError):
        return 0.0


def monster_at(p: Progress) -> tuple[int, int]:
    try:
        hp, armour = MONSTER_TABLE[p.book][p.chapter][p.enemy]
        return (int(hp), int(armour))
    except (IndexError, TypeError):
        return (0, 0)


_BASE_DAMAGE_TRANSITIONS: list[tuple[int, int, int]] = [
    (0, 0, 0),
    (0, 8, 4),
]


def base_damage_bonus_at(p: Progress) -> int:
    for book_idx, chapter_idx, base_damage in reversed(_BASE_DAMAGE_TRANSITIONS):
        if (p.book, p.chapter) >= (book_idx, chapter_idx):
            return base_damage
    return 0


CAT_METAL = 0
CAT_FELINE = 1
CAT_COLORS = 2
CAT_BONES = 3
CAT_FRUITSVEG = 4


def is_treasure_equipped(p: Progress) -> bool:
    return (p.book, p.chapter) >= (0, 8)


_WEAKNESSES: dict[tuple[int, int, int], tuple[int, float]] = {
    # METAL weakness
    (0, 7, 3): (CAT_METAL, 2.5),
    (0, 7, 4): (CAT_METAL, 2.75),
    (2, 3, 0): (CAT_METAL, 2.5),
    (2, 3, 1): (CAT_METAL, 2.5),
    (2, 3, 2): (CAT_METAL, 2.5),
    (2, 3, 3): (CAT_METAL, 2.5),
    (2, 3, 4): (CAT_METAL, 2.5),
    # FELINE weakness
    (0, 7, 5): (CAT_FELINE, 3.5),
    # COLORS weakness
    (0, 8, 6): (CAT_COLORS, 1.5),
    # BONES weakness
    (1, 2, 0): (CAT_BONES, 2.0),
    (1, 2, 1): (CAT_BONES, 2.0),
    (1, 2, 2): (CAT_BONES, 2.0),
    (1, 2, 4): (CAT_BONES, 2.0),
    (2, 7, 0): (CAT_BONES, 1.5),
    (2, 7, 1): (CAT_BONES, 1.5),
    (2, 7, 2): (CAT_BONES, 1.5),
    (2, 7, 3): (CAT_BONES, 1.5),
    (2, 7, 4): (CAT_BONES, 1.5),
    # FRUITSVEG weakness
    (1, 6, 0): (CAT_FRUITSVEG, 2.0),
    (1, 6, 1): (CAT_FRUITSVEG, 2.0),
    (1, 6, 2): (CAT_FRUITSVEG, 2.0),
    (1, 6, 3): (CAT_FRUITSVEG, 2.0),
    (1, 6, 4): (CAT_FRUITSVEG, 1.5),
    (2, 4, 0): (CAT_FRUITSVEG, 1.5),
    (2, 4, 1): (CAT_FRUITSVEG, 1.5),
    (2, 4, 2): (CAT_FRUITSVEG, 1.5),
    (2, 4, 3): (CAT_FRUITSVEG, 1.5),
    (2, 4, 4): (CAT_FRUITSVEG, 1.5),
}


def active_weakness(p: Progress) -> tuple[int, float]:
    return _WEAKNESSES.get((p.book, p.chapter, p.enemy), (-1, 1.0))


def active_chapter_preset(p: Progress) -> str:
    for book_idx, chapter_idx, key in reversed(_PRESET_TRANSITIONS):
        if (p.book, p.chapter) >= (book_idx, chapter_idx):
            return key
    return _PRESET_TRANSITIONS[0][2]


def advance(p: Progress) -> Progress:
    n_enemies = len(POWER_TABLE[p.book][p.chapter])
    if p.enemy + 1 < n_enemies:
        return Progress(p.book, p.chapter, p.enemy + 1)

    if p.chapter + 1 < len(POWER_TABLE[p.book]):
        return Progress(p.book, p.chapter + 1, 0)

    if p.book + 1 < len(POWER_TABLE):
        return Progress(p.book + 1, 0, 0)

    return Progress(p.book, p.chapter, p.enemy)


def retreat(p: Progress) -> Progress:
    if p.enemy > 0:
        return Progress(p.book, p.chapter, p.enemy - 1)
    if p.chapter > 0:
        new_chapter = p.chapter - 1
        return Progress(p.book, new_chapter, len(POWER_TABLE[p.book][new_chapter]) - 1)
    if p.book > 0:
        new_book = p.book - 1
        new_chapter = len(POWER_TABLE[new_book]) - 1
        return Progress(
            new_book, new_chapter, len(POWER_TABLE[new_book][new_chapter]) - 1
        )
    return p


def is_terminal(p: Progress) -> bool:
    return (
        p.book == len(POWER_TABLE) - 1
        and p.chapter == len(POWER_TABLE[p.book]) - 1
        and p.enemy == len(POWER_TABLE[p.book][p.chapter]) - 1
    )


def is_valid(book: int, chapter: int, enemy: int) -> bool:
    if book < 0 or book >= len(POWER_TABLE):
        return False
    if chapter < 0 or chapter >= len(POWER_TABLE[book]):
        return False
    if enemy < 0 or enemy >= len(POWER_TABLE[book][chapter]):
        return False
    return True


_NON_PERSIST_CHAPTERS: set[tuple[int, int]] = {
    (0, 6),
    (2, 9),
}


FIXED_OPENING_RACKS: dict[tuple[int, int], str | None] = {
    (0, 1): "ZANYPSAEFNURSITL",
    (0, 3): None,
}


def has_fixed_opening_rack(p: Progress) -> bool:
    return (p.book, p.chapter) in FIXED_OPENING_RACKS


_SCRIPTED_OPENINGS: dict[tuple[int, int, int], dict] = {
    (0, 0, 0): {"word": "PLAY", "positions": [4, 13, 2, 11]},
}


def scripted_opening(p: Progress) -> dict | None:
    return _SCRIPTED_OPENINGS.get((p.book, p.chapter, p.enemy))


_SPHINX_LOCATION: tuple[int, int] = (1, 3)  # 2.4
SPHINX_SCRIPT: list[str] = [
    ["SKY", "SKIES"],
    ["WALL", "WALLS"],
    ["FIST", "FISTS"],
    ["TRUTH", "TRUTHS"],
    ["WATER", "WATERS"]
]


def is_sphinx(p: Progress) -> bool:
    return (p.book, p.chapter) == _SPHINX_LOCATION


def sphinx_words_for_phase(phase: int) -> list[str] | None:
    if 0 <= phase < len(SPHINX_SCRIPT):
        return SPHINX_SCRIPT[phase]
    return None


def sphinx_upcoming_words(phase: int) -> list[str]:
    if phase < 0:
        phase = 0
    return sum(SPHINX_SCRIPT[phase + 1 :], [])


def fixed_opening_rack(p: Progress) -> str | None:
    return FIXED_OPENING_RACKS.get((p.book, p.chapter))


def next_chapter_start(p: Progress) -> Progress:
    nxt = advance(p)
    if nxt.chapter != p.chapter or nxt.book != p.book:
        return nxt
    if p.chapter + 1 < len(POWER_TABLE[p.book]):
        return Progress(p.book, p.chapter + 1, 0)
    if p.book + 1 < len(POWER_TABLE):
        return Progress(p.book + 1, 0, 0)
    return p


def rack_persists_into_next(p: Progress) -> bool:
    return (p.book, p.chapter) not in _NON_PERSIST_CHAPTERS


def is_same_rack_block(p: Progress) -> bool:
    return (p.book, p.chapter) in _NON_PERSIST_CHAPTERS


def same_rack_block_thresholds(p: Progress) -> list[tuple[int, int]]:
    if not is_same_rack_block(p):
        return []
    out: list[tuple[int, int]] = []
    row = MONSTER_TABLE[p.book][p.chapter]
    for e, (hp, armour) in enumerate(row):
        out.append((e, int(hp) * 4 + int(armour)))
    return out


def is_chapter_final(p: Progress) -> bool:
    return p.enemy == len(POWER_TABLE[p.book][p.chapter]) - 1


def is_terminal_kill(p: Progress) -> bool:
    if is_terminal(p):
        return True
    if not is_chapter_final(p):
        return False
    if not rack_persists_into_next(p):
        return True
    nxt = next_chapter_start(p)
    if has_fixed_opening_rack(nxt):
        return True
    return False


if __name__ == "__main__":
    p = Progress()
    n = 1
    while not is_terminal(p):
        next_p = advance(p)
        if next_p == p:
            break
        p = next_p
        n += 1
    print(f"Total enemies in game: {n}")
    print(
        f"Final position: {p.display()}, power={power_at(p)}, preset={active_chapter_preset(p)}"
    )

    print()
    print(f"  {'pos':>8}  power  preset  bonus  monster(hp,armour)")
    for b, c, e in [
        (0, 0, 0),
        (0, 4, 0),
        (0, 5, 0),
        (0, 7, 0),
        (0, 8, 0),
        (1, 5, 0),
        (1, 8, 0),
        (2, 9, 0),
    ]:
        pp = Progress(b, c, e)
        hp, armour = monster_at(pp)
        print(
            f"  {pp.display():>8}  {power_at(pp):>5.1f}  {active_chapter_preset(pp):<6}  "
            f"+{base_damage_bonus_at(pp)}     ({hp:>3}, {armour})"
        )
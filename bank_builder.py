"""Automated tile-bank builder (per-letter sweep).

Strategy:
    for each letter A-Z:
        repeat until we hit a streak of CLEAN_ASSIGNMENTS_TARGET assignments
        that each required zero corrections:
            1. Pick a random gem assignment (16 independent slots, each
               either "no gem" or one of the 7 gem types).
            2. Write a savestate forcing rack = LETTER * 16 with this gem
               assignment, then run the in-game user-switch sequence to
               make the game reload it.
            3. Capture, recognise, and verify each tile against ground
               truth. If anything disagrees, call correct_recognition
               (this tile, this letter, this gem) — we KNOW the answer
               because we just wrote the save.
            4. Repeat capture/verify until we hit CLEAN_READS_PER_ASSIGNMENT
               consecutive captures that needed no corrections.
            5. If any read in the assignment required corrections, the
               assignment is "dirty" — reset the streak; otherwise the
               assignment is "clean" and adds to the streak.

Run:
    python bank_builder.py [--letters AEIOU]
                           [--clean-assignments 5]
                           [--clean-reads-per-assignment 3]
                           [--max-reads-per-assignment 30]
                           [--max-assignments-per-letter 50]
                           [--dry-run] [-v]

Prerequisites:
    1. You're in-game, somewhere where the in-game user-switch sequence
       below can be invoked. A SAVESTATE.bwa file must already exist for
       your active user (start a chapter normally to create one).
    2. wordgame_calibration.json exists (the rack box is calibrated).
    3. wordgame.toml has a [bank_builder] section with:
        - save_path  — path to the existing .bwa save the script edits
          IN PLACE (preserves HP, items, position in chapter)
        - menu_exit, switch_user_1, confirm_1, switch_user_2, confirm_2
          (window-relative click coordinates)
    See bank_builder_config_example.toml for the full layout.

What this does NOT cover:
    Afflicted tiles (smashed, locked, plague) can't be produced by save-
    edit alone — they need actual gameplay. The right-click correction
    UI in the overlay handles them. The automation here covers the
    remaining 8 states (normal + 7 gems).
"""

from __future__ import annotations

import argparse
import json
import random
import string
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from PIL import Image

from capture import (
    RackBox,
    auto_detect_rack,
    capture_rack,
    ensure_calibration,
    find_game_window,
    split_tiles,
)
from config import load_config, resolve_path
from recognize import (
    Recognition,
    TileDB,
    correct_recognition,
    is_rack_paused_or_empty,
    recognise_rack,
)
from save_writer import override_rack_in_place


# ---------------------------------------------------------------------------
# Click sequences for reloading a save file
# ---------------------------------------------------------------------------


# All coordinates are RELATIVE to the game window's top-left corner. The
# automation finds the window and adds these as offsets at click time.
#
# TODO(user): fill these in via the [bank_builder] section in
# wordgame.toml. The defaults below are (0,0) placeholders; the script
# refuses to run with any of them unset.
#
# The sequence to reload an overridden save:
#   1. menu_exit    - leave the chapter to the user-select screen
#   2. switch_user_1 + confirm_1 - switch to a different user
#   3. switch_user_2 + confirm_2 - switch back (forces the save to reload)
DEFAULT_RELOAD_COORDS = {
    "menu_open": None,         # optional: only if you need a separate "open menu" click
    "menu_exit": (0, 0),       # TODO
    "menu_exit_confirm": (0, 0), # TODO
    "switch_user": (0, 0),   # TODO
    "new_user": (0, 0),
    "confirm": (0, 0),       # TODO
    "resume_play": None,       # optional: only if step 6 doesn't land at the play screen
}


@dataclass
class ReloadConfig:
    """Click coordinates and delays for the in-game user-switch sequence."""

    coords: dict

    inter_click_delay1: float = 0.10
    inter_click_delay2: float = 1.10
    # Time to wait after the final reload click before capturing the rack.
    # The game needs a moment to redraw the play screen.
    settle_delay: float = 2.0
    mouse_speed: float = 0.05




def _click(window, x: int, y: int, *, button: str = "left",
           mouse_speed: float = 0.05) -> None:
    """Click at window-relative (x, y)."""
    import pyautogui
    pyautogui.click(
        window.left + x, window.top + y, button=button, duration=mouse_speed,
    )


def run_reload_sequence(window, reload_cfg: ReloadConfig, save_path, rack, gems, enable_gems) -> None:
    """Execute the menu-exit -> switch-user -> switch-user-back sequence.

    Raises RuntimeError if any required coord is still at the (0,0)
    placeholder - fail loudly rather than click in the wrong place.
    """
    sequence_keys = (
        ("menu_open", reload_cfg.inter_click_delay1),
        ("menu_exit", reload_cfg.inter_click_delay1),
        ("menu_exit_confirm", reload_cfg.inter_click_delay2),
        ("overwrite_rack", None),
        ("switch_user", reload_cfg.inter_click_delay1),
        ("new_user", reload_cfg.inter_click_delay1),
        ("confirm", reload_cfg.inter_click_delay2),
        ("switch_user", reload_cfg.inter_click_delay1),
        ("new_user", reload_cfg.inter_click_delay1),
        ("confirm", reload_cfg.inter_click_delay2),
        ("resume_play", reload_cfg.inter_click_delay2),
    )
    coords = reload_cfg.coords
    for (key, delay) in sequence_keys:
        target = coords.get(key)
        if key == "overwrite_rack":
            override_rack_in_place(
                save_path=save_path,
                rack=rack,
                gems=gems,
                enable_gems=enable_gems,
            )
        else:
            if target is None:
                continue
            if target == (0, 0):
                raise RuntimeError(
                    f"reload coord {key!r} is unset (still at the (0,0) placeholder). "
                    "Fill in the [bank_builder] section of wordgame.toml or edit "
                    "DEFAULT_RELOAD_COORDS in bank_builder.py."
                )
            x, y = target
            _click(window, x, y, mouse_speed=reload_cfg.mouse_speed)
            time.sleep(delay)
    time.sleep(reload_cfg.settle_delay)



# ---------------------------------------------------------------------------
# Gem assignment generation
# ---------------------------------------------------------------------------


# Mapping from the editor's 0..7 gem int to the state string recognize uses.
# Order matches save_writer._BINARY_GEMS.
_EDITOR_GEMS = (
    "amethyst", "emerald", "garnet", "sapphire", "ruby", "crystal", "diamond",
)


def _gem_int_to_state(g: int) -> str:
    """Convert the editor's 0..7 gem int to a recognize state label."""
    if g == 0:
        return "normal"
    return _EDITOR_GEMS[g - 1]


def random_gem_assignment(rng: random.Random) -> list[int]:
    """16 independently-rolled slots, each either no-gem or one of 7 gem types.

    Each slot is independent: ~12.5% chance of each of the 8 outcomes.
    That's denser than a real rack (where gems are rare), but it's how
    we cover every (letter, gem) combination efficiently.
    """
    return [int((x:=rng.randint(0, 30)) * (x <= 7)) for _ in range(16)]


# ---------------------------------------------------------------------------
# Verify-and-correct (one capture)
# ---------------------------------------------------------------------------


def _expected_state_for(letter: str, gem_int: int) -> str:
    return _gem_int_to_state(gem_int)


def _matches_truth(rec: Recognition, expected_letter: str,
                   expected_state: str) -> bool:
    return rec.letter == expected_letter and rec.state == expected_state


def verify_and_correct(
    tiles: list[Image.Image],
    recognitions: list[Recognition],
    rack: str,
    gems: list[int],
    db: TileDB,
    *,
    verbose: bool = False,
) -> int:
    """Compare each recognition to ground truth and write corrections to bank.

    Returns the number of tiles that disagreed (and were corrected). A return
    value of 0 means this read was perfectly clean.
    """
    n_corrections = 0
    for i, (tile_img, rec) in enumerate(zip(tiles, recognitions)):
        expected_letter = rack[i].upper()
        expected_state = _expected_state_for(expected_letter, gems[i])
        if _matches_truth(rec, expected_letter, expected_state):
            continue
        try:
            correct_recognition(tile_img, db, expected_letter, expected_state)
        except ValueError as e:
            print(f"  ! tile {i}: correct_recognition refused: {e}")
            continue
        n_corrections += 1
        if verbose:
            print(
                f"    tile {i:2d}: expected {expected_letter}/{expected_state}, "
                f"got {rec.letter or '?'}/{rec.state} (dist {rec.distance:.1f}) "
                f"-> bank updated"
            )
    return n_corrections


# ---------------------------------------------------------------------------
# One gem-assignment cycle for a fixed letter
# ---------------------------------------------------------------------------


@dataclass
class AssignmentResult:
    gems: list[int]
    needed_correction: bool
    reads_used: int
    corrections_applied: int
    aborted: bool = False     # True if we hit max_reads_per_assignment


def process_assignment(
    letter: str,
    gems: list[int],
    db: TileDB,
    box: RackBox,
    window,
    reload_cfg: ReloadConfig,
    *,
    save_path: Path,
    clean_reads_target: int,
    max_reads: int,
    enable_gems: bool,
    dry_run: bool,
    verbose: bool,
) -> AssignmentResult:
    """Drive one (letter, gem-assignment) cycle.

    Overrides the existing save file's rack in place, runs the reload
    sequence, then loops:
        capture -> verify -> if dirty, correct & reset streak; if clean,
        increment streak; stop when streak == clean_reads_target or we
        exceed max_reads.
    """
    rack = letter * 16
    if verbose:
        print(f"  gems={gems}")

    if not dry_run:
        run_reload_sequence(window, reload_cfg, save_path, rack, gems, enable_gems)

    needed_correction = False
    consecutive_clean = 0
    total_corrections = 0

    for read_idx in range(1, max_reads + 1):
        try:
            rack_img = capture_rack(box)
            tiles = split_tiles(rack_img)
        except Exception as e:
            print(f"  ! capture failed at read {read_idx}: {e}")
            time.sleep(1.0)
            continue

        if is_rack_paused_or_empty(tiles):
            print(f"  ! rack paused/empty at read {read_idx}; sleeping then retrying.")
            time.sleep(1.0)
            consecutive_clean = 0
            continue

        recs = recognise_rack(tiles, db)
        if dry_run:
            shown = " ".join(
                f"{(r.letter or '?')}/{r.state[:3]}" for r in recs
            )
            print(f"  read {read_idx}: {shown}")
            consecutive_clean += 1
            if consecutive_clean >= clean_reads_target:
                return AssignmentResult(
                    gems=gems, needed_correction=False,
                    reads_used=read_idx, corrections_applied=0,
                )
            continue

        n_corr = verify_and_correct(
            tiles, recs, rack, gems, db, verbose=verbose,
        )
        total_corrections += n_corr

        if n_corr == 0:
            consecutive_clean += 1
            if verbose:
                print(
                    f"  read {read_idx}: clean ({consecutive_clean}/{clean_reads_target})"
                )
            if consecutive_clean >= clean_reads_target:
                return AssignmentResult(
                    gems=gems,
                    needed_correction=needed_correction,
                    reads_used=read_idx,
                    corrections_applied=total_corrections,
                )
        else:
            needed_correction = True
            consecutive_clean = 0
            if verbose:
                print(
                    f"  read {read_idx}: {n_corr} tile(s) corrected; streak reset"
                )
        time.sleep(0.1)

    # Ran out of reads without hitting the consecutive-clean target.
    return AssignmentResult(
        gems=gems,
        needed_correction=True,  # treat as dirty if we couldn't stabilise
        reads_used=max_reads,
        corrections_applied=total_corrections,
        aborted=True,
    )


# ---------------------------------------------------------------------------
# Outer loop: one letter
# ---------------------------------------------------------------------------


@dataclass
class LetterStats:
    letter: str
    assignments_attempted: int = 0
    assignments_clean: int = 0
    consecutive_clean_at_finish: int = 0
    total_reads: int = 0
    total_corrections: int = 0
    aborted_assignments: int = 0


def process_letter(
    letter: str,
    db: TileDB,
    box: RackBox,
    window,
    reload_cfg: ReloadConfig,
    *,
    rng: random.Random,
    save_path: Path,
    clean_assignments_target: int,
    clean_reads_per_assignment: int,
    max_reads_per_assignment: int,
    max_assignments_per_letter: int,
    enable_gems: bool,
    dry_run: bool,
    verbose: bool,
) -> LetterStats:
    """Drive one letter: spin assignments until the streak target is hit."""
    print(f"\n=== Letter {letter} ===")
    stats = LetterStats(letter=letter)
    streak = 0

    while streak < clean_assignments_target:
        if stats.assignments_attempted >= max_assignments_per_letter:
            print(
                f"  Letter {letter}: hit max_assignments_per_letter "
                f"({max_assignments_per_letter}) with streak={streak}/"
                f"{clean_assignments_target}. Moving on."
            )
            break

        gems = random_gem_assignment(rng)
        stats.assignments_attempted += 1
        print(
            f"  assignment {stats.assignments_attempted} "
            f"(streak {streak}/{clean_assignments_target})"
        )

        result = process_assignment(
            letter=letter,
            gems=gems,
            db=db,
            box=box,
            window=window,
            reload_cfg=reload_cfg,
            save_path=save_path,
            clean_reads_target=clean_reads_per_assignment,
            max_reads=max_reads_per_assignment,
            enable_gems=enable_gems,
            dry_run=dry_run,
            verbose=verbose,
        )
        stats.total_reads += result.reads_used
        stats.total_corrections += result.corrections_applied
        if result.aborted:
            stats.aborted_assignments += 1

        if result.needed_correction:
            streak = 0
            if not result.aborted:
                print(
                    f"    {result.corrections_applied} corrections, "
                    f"{result.reads_used} reads; streak reset"
                )
            else:
                print(
                    f"    ABORTED after {result.reads_used} reads "
                    f"({result.corrections_applied} corrections); streak reset"
                )
        else:
            streak += 1
            stats.assignments_clean += 1
            print(
                f"    clean in {result.reads_used} reads; "
                f"streak {streak}/{clean_assignments_target}"
            )

    stats.consecutive_clean_at_finish = streak
    print(
        f"=== Letter {letter} done: {stats.assignments_clean}/"
        f"{stats.assignments_attempted} clean, "
        f"{stats.total_corrections} corrections written"
    )
    return stats


# ---------------------------------------------------------------------------
# Outer-outer loop + resume
# ---------------------------------------------------------------------------


@dataclass
class RunState:
    """Persists per-letter progress so we can resume after Ctrl+C / crash."""

    completed_letters: list[str] = field(default_factory=list)
    per_letter_stats: list[dict] = field(default_factory=list)

    @classmethod
    def load(cls, path: Path) -> "RunState":
        if not path.exists():
            return cls()
        raw = json.loads(path.read_text())
        return cls(
            completed_letters=raw.get("completed_letters", []),
            per_letter_stats=raw.get("per_letter_stats", []),
        )

    def save(self, path: Path) -> None:
        path.write_text(
            json.dumps(
                {
                    "completed_letters": self.completed_letters,
                    "per_letter_stats": self.per_letter_stats,
                },
                indent=2,
            )
        )


def run(
    letters: str,
    *,
    box: RackBox,
    window,
    db: TileDB,
    reload_cfg: ReloadConfig,
    save_path: Path,
    state_path: Path,
    rng: random.Random,
    clean_assignments_target: int,
    clean_reads_per_assignment: int,
    max_reads_per_assignment: int,
    max_assignments_per_letter: int,
    enable_gems: bool,
    dry_run: bool,
    verbose: bool,
    resume: bool,
) -> RunState:
    state = RunState.load(state_path) if resume else RunState()
    if resume and state.completed_letters:
        print(f"Resuming; already done: {''.join(state.completed_letters)}")

    try:
        for letter in letters:
            letter = letter.upper()
            if letter in state.completed_letters:
                print(f"--- {letter} already done; skipping ---")
                continue
            stats = process_letter(
                letter=letter,
                db=db,
                box=box,
                window=window,
                reload_cfg=reload_cfg,
                rng=rng,
                save_path=save_path,
                clean_assignments_target=clean_assignments_target,
                clean_reads_per_assignment=clean_reads_per_assignment,
                max_reads_per_assignment=max_reads_per_assignment,
                max_assignments_per_letter=max_assignments_per_letter,
                enable_gems=enable_gems,
                dry_run=dry_run,
                verbose=verbose,
            )
            state.completed_letters.append(letter)
            state.per_letter_stats.append(asdict(stats))
            state.save(state_path)
    except KeyboardInterrupt:
        print("\nInterrupted - partial progress saved.")
        state.save(state_path)
    return state


# ---------------------------------------------------------------------------
# Config loading + CLI
# ---------------------------------------------------------------------------


def _load_reload_cfg(cfg) -> ReloadConfig:
    """Pull reload coords + delays from the [bank_builder] section."""
    coords = dict(DEFAULT_RELOAD_COORDS)
    section = (cfg or {}).get("bank_builder", {})
    for key in (
        "menu_open", "menu_exit", "menu_exit_confirm",
        "switch_user", "new_user", "confirm",
        "switch_user", "new_user", "confirm",
        "resume_play",
    ):
        raw = section.get(key)
        if raw is None or raw == "":
            if raw == "":
                coords[key] = None
            continue
        if isinstance(raw, str):
            parts = [p.strip() for p in raw.split(",")]
            if len(parts) != 2:
                raise ValueError(f"{key!r} should be 'x,y'; got {raw!r}")
            coords[key] = (int(parts[0]), int(parts[1]))
        elif isinstance(raw, (list, tuple)) and len(raw) == 2:
            coords[key] = (int(raw[0]), int(raw[1]))
        else:
            raise ValueError(f"{key!r} should be 'x,y' or [x, y]; got {raw!r}")
    return ReloadConfig(
        coords=coords,
        inter_click_delay1=section.get("inter_click_delay1", 0.10),
        inter_click_delay2=section.get("inter_click_delay2", 1.10),
        settle_delay=section.get("settle_delay", 2.0),
        mouse_speed=section.get("mouse_speed", 0.05),
    )


def main(argv: list[str] | None = None) -> int:
    time.sleep(2)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--letters", default=string.ascii_uppercase,
        help="Which letters to sweep, in order. Default: A-Z.",
    )
    ap.add_argument(
        "--clean-assignments", type=int, default=5,
        help="Consecutive clean assignments required per letter (default 5).",
    )
    ap.add_argument(
        "--clean-reads-per-assignment", type=int, default=3,
        help="Consecutive clean reads within an assignment (default 3).",
    )
    ap.add_argument(
        "--max-reads-per-assignment", type=int, default=30,
        help="Hard cap on capture/verify iterations per assignment.",
    )
    ap.add_argument(
        "--max-assignments-per-letter", type=int, default=50,
        help="Hard cap on assignments per letter (in case the streak target "
        "is unreachable).",
    )
    ap.add_argument(
        "--seed", type=int, default=None,
        help="Random seed for gem-assignment generation.",
    )
    ap.add_argument(
        "--enable-gems", action="store_true", default=True,
        help="Set the 'gems can spawn' flag in the save (default).",
    )
    ap.add_argument(
        "--no-gems", dest="enable_gems", action="store_false",
        help="Disable the 'gems can spawn' flag.",
    )
    ap.add_argument(
        "--save",
        help="Path to the active .bwa save file the script edits in place. "
        "Overrides config.",
    )
    ap.add_argument(
        "--state-file", default="bank_builder_state.json",
        help="Where to persist per-letter progress (for resume).",
    )
    ap.add_argument(
        "--resume", action="store_true",
        help="Skip letters already in the state file.",
    )
    ap.add_argument(
        "--dry-run", action="store_true",
        help="Don't write save files or click; just go through the capture "
        "loop and log what we see. The bank is NOT modified.",
    )
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args(argv)

    cfg = load_config("wordgame.toml")

    save_arg = args.save or (
        (cfg or {}).get("bank_builder", {}).get("save_path")
    )
    if save_arg is None:
        print(
            "ERROR: no save path. Pass --save or set "
            "[bank_builder].save_path in wordgame.toml. The path should "
            "point at your active SAVESTATE.bwa file — the script edits "
            "it in place, preserving your HP / chapter position.",
            file=sys.stderr,
        )
        return 2
    save_path = resolve_path(save_arg)
    if not save_path.exists():
        print(
            f"ERROR: save_path {save_path} does not exist. Start a game "
            "in the target chapter first so the save file is created.",
            file=sys.stderr,
        )
        return 2
    state_path = resolve_path(args.state_file)

    if not args.dry_run:
        window = find_game_window()
        if window is None:
            print("ERROR: game window not found.", file=sys.stderr)
            return 2
    else:
        @dataclass
        class _StubWin:
            left: int = 0
            top: int = 0
        window = _StubWin()

    box = auto_detect_rack() or ensure_calibration()
    if box is None:
        print("ERROR: could not establish rack calibration.", file=sys.stderr)
        return 2

    reload_cfg = _load_reload_cfg(cfg)
    db = TileDB()
    rng = random.Random(args.seed)

    letters = args.letters.upper()
    invalid = [c for c in letters if not c.isalpha()]
    if invalid:
        print(f"ERROR: invalid letters {invalid!r}; use A-Z only.", file=sys.stderr)
        return 2

    print(
        f"Plan: letters={letters}, "
        f"clean_assignments={args.clean_assignments}, "
        f"clean_reads_per_assignment={args.clean_reads_per_assignment}, "
        f"max_reads_per_assignment={args.max_reads_per_assignment}, "
        f"max_assignments_per_letter={args.max_assignments_per_letter}"
    )
    print(f"Save file:  {save_path}")
    print(f"State file: {state_path}")
    print(f"Bank size:  {len(db)} entries before run")

    state = run(
        letters,
        box=box,
        window=window,
        db=db,
        reload_cfg=reload_cfg,
        save_path=save_path,
        state_path=state_path,
        rng=rng,
        clean_assignments_target=args.clean_assignments,
        clean_reads_per_assignment=args.clean_reads_per_assignment,
        max_reads_per_assignment=args.max_reads_per_assignment,
        max_assignments_per_letter=args.max_assignments_per_letter,
        enable_gems=args.enable_gems,
        dry_run=args.dry_run,
        verbose=args.verbose,
        resume=args.resume,
    )

    print()
    print("=== Done ===")
    print(f"Letters completed: {''.join(state.completed_letters)}")
    print(f"Bank size after run: {len(db)} entries")
    total_reads = sum(s.get("total_reads", 0) for s in state.per_letter_stats)
    total_corr = sum(s.get("total_corrections", 0) for s in state.per_letter_stats)
    print(f"Total reads:       {total_reads}")
    print(f"Total corrections: {total_corr}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
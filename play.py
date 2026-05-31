from __future__ import annotations

import pyautogui
import time
from functools import wraps
from collections.abc import Sequence

from engine import Tile
from recognize import Recognition
from capture import RackBox


MENU_COORDS = (666, 606)
MENU_QUIT_COORDS = (424, 340)
MENU_QUIT_CONFIRM_COORDS = (334, 413)
GAME_START_COORDS = (237, 346)

def reset_position(func):
    @wraps(func)
    def wrapper(*args, **kwargs):
        x, y = pyautogui.position()
        func(*args, **kwargs)
        pyautogui.moveTo(x, y)
    return wrapper


def resolve_positions(
    word_tiles: Sequence[Tile], rack_recs: Sequence[Recognition]
) -> list[int] | None:
    used: set[int] = set()
    positions: list[int] = []

    for wt in word_tiles:
        target_letter = wt.letter.upper()
        target_gem = wt.gem
        chosen: int | None = None

        for i, rec in enumerate(rack_recs):
            if i in used:
                continue
            if rec.status != "normal":
                continue
            if rec.letter == target_letter and rec.gem == target_gem:
                chosen = i
                break

        if chosen is None:
            for i, rec in enumerate(rack_recs):
                if i in used:
                    continue
                if rec.status != "normal":
                    continue
                if rec.letter == target_letter:
                    chosen = i
                    break

        if chosen is None:
            return None

        used.add(chosen)
        positions.append(chosen)

    return positions

@reset_position
def play_word(
    positions: Sequence[int],
    rack_box: RackBox,
    *,
    click_delay: float = 0.10,
    mouse_speed: float = 0.05,
    post_word_delay: float = 0.10,
    powered: bool = False,
) -> None:
    pyautogui.FAILSAFE = True

    if powered:
        pyautogui.press("2")

    for idx in positions:
        row, col = idx // 4, idx % 4
        l, t, r, b = rack_box.tile_bbox(row, col)
        cx, cy = (l + r) // 2, (t + b) // 2
        pyautogui.click(cx, cy, duration=mouse_speed)
        if click_delay > 0:
            time.sleep(click_delay)

    if post_word_delay > 0:
        time.sleep(post_word_delay)

    pyautogui.press("enter")

@reset_position
def reset_game(
    window_left: int,
    window_top: int,
    *,
    menu_delay: float = 0.5,
    quit_delay: float = 0.5,
    confirm_delay: float = 1.0,
    mouse_speed: float = 0.05,
) -> None:

    pyautogui.FAILSAFE = True

    steps = [
        (MENU_COORDS, menu_delay),
        (MENU_QUIT_COORDS, quit_delay),
        (MENU_QUIT_CONFIRM_COORDS, confirm_delay),
        (GAME_START_COORDS, 0.0),
    ]

    for (x, y), delay in steps:
        pyautogui.click(window_left + x, window_top + y, duration=mouse_speed)
        if delay > 0:
            time.sleep(delay)

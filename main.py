from __future__ import annotations

import argparse
import sys
import time

from engine import Engine
from chapter import CHAPTERS
from capture import (
    ensure_calibration,
    capture_rack,
    split_tiles,
    find_game_window,
    DEFAULT_RACK_OFFSET_X,
    DEFAULT_RACK_OFFSET_Y,
    DEFAULT_TILE_SIZE_X,
    DEFAULT_TILE_SIZE_Y,
)
from recognize import (
    TileDB,
    recognise_rack,
    label_unknowns_interactively,
    Recognition,
    is_rack_paused_or_empty,
)
from overlay import Overlay
from play import resolve_positions, play_word, reset_game
from config import load_config, resolve_path, pick
from progress import (
    Progress,
    load_progress,
    save_progress,
    power_at,
    monster_at,
    base_damage_bonus_at,
    is_treasure_equipped,
    active_weakness,
    active_chapter_preset,
    rack_persists_into_next,
    advance as progress_advance,
    retreat as progress_retreat,
    is_terminal as progress_is_terminal,
    is_valid as progress_is_valid,
)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Bookworm Adventures word suggester")
    ap.add_argument("--config", default=None, help="Path to wordgame.toml.")
    ap.add_argument("--exe", default=None, help="Path to wordgame.exe")
    ap.add_argument("--dict", default=None, help="Path to dictionary file")
    ap.add_argument("--chapter", default=None, help="Chapter preset")
    ap.add_argument("--manual-calibrate", action="store_true", default=None)
    ap.add_argument("--rack-offset-x", type=int, default=None)
    ap.add_argument("--rack-offset-y", type=int, default=None)
    ap.add_argument("--tile-size-x", type=int, default=None)
    ap.add_argument("--tile-size-y", type=int, default=None)
    ap.add_argument("--n", type=int, default=None)
    ap.add_argument("--horizon", type=int, default=None)
    ap.add_argument("--max-sims", type=int, default=None)
    ap.add_argument(
        "--focus-click", action=argparse.BooleanOptionalAction, default=None
    )
    ap.add_argument("--focus-delay", type=float, default=None)
    ap.add_argument("--overlay-x", default=None)
    ap.add_argument("--overlay-y", default=None)
    ap.add_argument("--click-delay", type=float, default=None)
    ap.add_argument("--pre-play-delay", type=float, default=None)
    ap.add_argument("--mouse-move-speed", type=float, default=None)
    return ap.parse_args()


def position_overlay_next_to_rack(
    overlay: "Overlay",
    box,
    ba_window=None,
    override_x: int | None = None,
    override_y: int | None = None,
) -> None:
    """Place the overlay so it doesn't sit on top of the game."""
    overlay.root.update_idletasks()
    sw = overlay.root.winfo_screenwidth()
    sh = overlay.root.winfo_screenheight()
    ow = overlay.WIDTH
    oh = overlay.root.winfo_reqheight() or 600

    if override_x is not None or override_y is not None:
        x = override_x if override_x is not None else 20
        y = override_y if override_y is not None else 20
        overlay.root.geometry(f"{ow}x{oh}+{int(x)}+{int(y)}")
        overlay.root.update()
        return

    pad = 20

    if ba_window is not None:
        game_left = ba_window.left
        game_right = ba_window.left + ba_window.width
        x = game_right + pad
        y = ba_window.top
    else:
        game_left = box.left - 304
        game_right = game_left + 800
        x = game_right + pad
        y = max(20, box.top - 40)

    if x + ow > sw:
        x_left = game_left - ow - pad
        if x_left >= 0:
            x = x_left
        else:
            x = max(0, sw - ow - pad)

    if y + oh > sh:
        y = max(0, sh - oh - 20)

    overlay.root.geometry(f"{ow}x{oh}+{int(x)}+{int(y)}")
    overlay.root.update()


def main() -> int:
    args = parse_args()
    cfg = load_config(args.config)

    chapter = pick(args, "chapter", cfg, "engine", "chapter", "1.1")
    n_words = pick(args, "n", cfg, "engine", "n", 8)
    horizon = pick(args, "horizon", cfg, "engine", "horizon", 2)
    max_sims = pick(args, "max_sims", cfg, "engine", "max_sims", 300)
    exe_raw = pick(args, "exe", cfg, "engine", "exe", None)
    dict_raw = pick(args, "dict", cfg, "engine", "dict", None)

    if not exe_raw or not dict_raw:
        print("Need --exe and --dict", file=sys.stderr)
        return 1

    exe_path = resolve_path(exe_raw)
    dict_path = resolve_path(dict_raw)

    manual_cal = pick(args, "manual_calibrate", cfg, "rack", "manual_calibrate", False)
    rack_offset_x = pick(
        args, "rack_offset_x", cfg, "rack", "rack_offset_x", DEFAULT_RACK_OFFSET_X
    )
    rack_offset_y = pick(
        args, "rack_offset_y", cfg, "rack", "rack_offset_y", DEFAULT_RACK_OFFSET_Y
    )
    tile_size_x = pick(
        args, "tile_size_x", cfg, "rack", "tile_size_x", DEFAULT_TILE_SIZE_X
    )
    tile_size_y = pick(
        args, "tile_size_y", cfg, "rack", "tile_size_y", DEFAULT_TILE_SIZE_Y
    )

    focus_click = pick(args, "focus_click", cfg, "capture", "focus_click", True)
    focus_click_x = pick(args, None, cfg, "capture", "focus_click_x", 280)
    focus_click_y = pick(args, None, cfg, "capture", "focus_click_y", 460)
    focus_click_button = pick(args, None, cfg, "capture", "focus_click_button", "right")
    focus_settle_delay = pick(args, None, cfg, "capture", "focus_settle_delay", 0.6)
    focus_delay = pick(args, "focus_delay", cfg, "capture", "focus_delay", 3.0)

    click_delay = pick(args, "click_delay", cfg, "play", "click_delay", 0.10)
    pre_play_delay = pick(args, "pre_play_delay", cfg, "play", "pre_delay", 0.40)
    mouse_move_speed = pick(
        args, "mouse_move_speed", cfg, "play", "mouse_move_speed", 0.05
    )
    post_word_delay = pick(args, None, cfg, "play", "post_word_delay", 0.10)

    reset_menu_delay = pick(args, None, cfg, "reset", "menu_delay", 0.5)
    reset_quit_delay = pick(args, None, cfg, "reset", "quit_delay", 0.5)
    reset_confirm_delay = pick(args, None, cfg, "reset", "confirm_delay", 1.0)

    delay_read = pick(args, None, cfg, "reset", "delay_read", 0.5)
    delay_kill_to_reset = pick(args, None, cfg, "reset", "delay_kill_to_reset", 2.0)
    delay_overkill_to_reset = pick(
        args, None, cfg, "reset", "delay_overkill_to_reset", 3.0
    )
    delay_post_reset = pick(args, None, cfg, "reset", "delay_post_reset", 0.0)

    overlay_x_raw = pick(args, "overlay_x", cfg, "overlay", "x", "auto")
    overlay_y_raw = pick(args, "overlay_y", cfg, "overlay", "y", "auto")
    overlay_x = None if overlay_x_raw == "auto" else int(overlay_x_raw)
    overlay_y = None if overlay_y_raw == "auto" else int(overlay_y_raw)

    overlay = Overlay(title="BA Solver")
    overlay.set_status("Starting up — detecting BA window...")
    overlay.root.geometry("+10+10")
    overlay.root.update()

    ba_window = None
    box = None
    if not manual_cal:
        try:
            ba_window = find_game_window()
            if ba_window is not None:
                from capture import rack_box_from_window

                box = rack_box_from_window(
                    ba_window, rack_offset_x, rack_offset_y, tile_size_x, tile_size_y
                )
        except Exception as e:
            print(f"Window auto-detect failed: {e}", file=sys.stderr)

    if box is None:
        box = ensure_calibration(
            force=True, parent=overlay.root, focus_delay=focus_delay
        )

    position_overlay_next_to_rack(
        overlay, box, ba_window=ba_window, override_x=overlay_x, override_y=overlay_y
    )
    overlay.root.attributes("-topmost", False)
    overlay.root.deiconify()

    eng = Engine(str(exe_path), str(dict_path))
    db = TileDB()

    manual_chapter_override = args.chapter is not None
    if manual_chapter_override:
        if args.chapter not in CHAPTERS:
            print(f"Unknown chapter '{args.chapter}'.", file=sys.stderr)
            return 1
        prog = Progress()
        eng.config(**CHAPTERS[args.chapter])
    else:
        prog = load_progress()

    def push_engine_config_for_progress() -> None:
        if manual_chapter_override:
            return
        preset = active_chapter_preset(prog)
        # Read power from the overlay rather than from prog directly. On
        # progress changes, autofill_fields_for_current_enemy() runs first
        # and sets the overlay power to power_at(prog), so the value here
        # matches the new enemy. When the user manually edits the power
        # field (and fires on_power_changed → here), we use their value
        # instead. This makes the field a true override.
        power = overlay.get_power()
        base_damage_bonus = base_damage_bonus_at(prog)
        _, armour = monster_at(prog)
        treasure_equipped = is_treasure_equipped(prog)
        weakness_cat, weakness_boost = active_weakness(prog)
        eng.config(
            **CHAPTERS[preset],
            power=power,
            base_damage_bonus=base_damage_bonus,
            enemy_armour=armour,
            treasure_equipped=treasure_equipped,
            weakness_cat=weakness_cat,
            weakness_boost=weakness_boost,
        )

    # Prime the overlay's power field with the default for the current
    # progress before the first config push. push_engine_config_for_progress
    # reads overlay.get_power(), so we need the overlay primed first or
    # we'd send power=0 to the engine on startup. The full autofill (HP
    # + power) runs later — this is just the bit we need pre-push.
    overlay.set_power(power_at(prog))
    push_engine_config_for_progress()

    def refresh_progress_display() -> None:
        if manual_chapter_override:
            overlay.update_progress(
                f"chapter override: {args.chapter}  (progress disabled)", at_end=True
            )
        else:
            preset = active_chapter_preset(prog)
            power = power_at(prog)
            overlay.update_progress(
                f"📍 {prog.display()}   power {power:+.1f}%   preset {preset}",
                at_end=progress_is_terminal(prog),
            )

    gem_chars = {
        "none": " ",
        "amethyst": "a",
        "emerald": "e",
        "sapphire": "s",
        "garnet": "g",
        "ruby": "r",
        "crystal": "c",
        "diamond": "d",
    }

    latest_recs: list[list[Recognition]] = [[]]
    run_state = {"auto": False}

    def focus_game_window(pre_delay: float = 0.40) -> None:
        try:
            import pyautogui
        except ImportError:
            return
        win = find_game_window()
        if win is None:
            return

        x = win.left + focus_click_x
        y = win.top + focus_click_y
        pyautogui.click(x, y, button=focus_click_button, duration=mouse_move_speed)

        if pre_delay > 0:
            time.sleep(pre_delay)

    def do_capture_and_search() -> None:
        try:
            overlay.set_status("Capturing...")
            overlay.root.update_idletasks()

            rack_img = capture_rack(box)
            tiles = split_tiles(rack_img)

            if is_rack_paused_or_empty(tiles):
                overlay.set_status(
                    "Rack paused/unfocused — click the game and press R."
                )
                run_state["auto"] = False
                return

            recs = recognise_rack(tiles, db)
            overlay.update_rack(recs)

            if any(not r.confident for r in recs):
                overlay.root.attributes("-topmost", True)
                overlay.set_status("Labelling unknown tiles...")
                overlay.root.update_idletasks()

                lx = overlay.root.winfo_x()
                ly = overlay.root.winfo_y() + overlay.root.winfo_height() + 20
                sh = overlay.root.winfo_screenheight()
                if ly + 400 > sh:
                    ly = max(0, sh - 420)

                recs = label_unknowns_interactively(
                    tiles, recs, db, parent=overlay.root, geometry=f"+{lx}+{ly}"
                )
                overlay.update_rack(recs)
                overlay.root.attributes("-topmost", False)

            playable = [r for r in recs if r.status == "normal"]
            non_normal = [r for r in recs if r.status != "normal"]
            if non_normal:
                breakdown = ", ".join(sorted({r.status for r in non_normal}))
                print(f"Excluding {len(non_normal)} non-playable tiles ({breakdown})")

            if any(not r.letter for r in playable):
                overlay.set_status("Some playable tiles unknown; skipping search.")
                run_state["auto"] = False
                return

            if not playable:
                overlay.set_status("No playable tiles on rack.")
                run_state["auto"] = False
                return

            latest_recs[0] = recs
            letters: str = "".join(r.letter for r in playable)
            gems = "".join(gem_chars.get(r.gem, " ") for r in playable)

            overlay.set_status(f"Searching ({letters})...")
            overlay.root.update_idletasks()

            threshold = overlay.get_threshold()
            charges = overlay.get_charges()
            words = eng.top(
                letters,
                gems=gems,
                n=n_words,
                horizon=horizon,
                max_sims=max_sims,
                threshold=threshold,
                charges=charges,
            )
            overlay.update_words(words)

            if words:
                top = words[0]
                marker_pow = " 💪" if top.used_powered else ""
                if top.is_kill:
                    overkill = top.now - threshold
                    overkill_str = (
                        f", +{overkill} overkill" if overkill > 0 else ", exact"
                    )
                    overlay.set_status(
                        f"💀 KILL: {top.word}{marker_pow} ({top.now}{overkill_str}, leave {top.future:.1f})"
                    )
                else:
                    overlay.set_status(
                        f"Top: {top.word}{marker_pow} ({top.now}, total {top.total:.1f})"
                    )

                if run_state["auto"]:
                    if top.is_kill:
                        overlay.schedule(50, lambda w=top: handle_play(w))
                    else:
                        run_state["auto"] = False
                        overlay.set_status(
                            f"Auto halted: no kill available (best is {top.word} at {top.now} damage)."
                        )
            else:
                overlay.set_status("No words found.")
                run_state["auto"] = False

        except Exception as e:
            overlay.set_status(f"Error: {e}")
            run_state["auto"] = False

    def handle_play(word) -> None:
        recs = latest_recs[0]
        if not recs:
            overlay.set_status("No rack recognized — press R.")
            run_state["auto"] = False
            return

        positions = resolve_positions(word.tiles, recs)
        if positions is None:
            overlay.set_status("Couldn't place letters. Rack changed?")
            run_state["auto"] = False
            return

        overlay.set_status(f"Focusing window to play '{word.word}'...")
        overlay.root.update_idletasks()
        if focus_click:
            focus_game_window(pre_play_delay)

        overlay.set_status(f"Playing '{word.word}'...")
        overlay.root.update_idletasks()

        try:
            word_gems = "".join(gem_chars.get(t.gem, " ") for t in word.tiles)
            trace_resp = eng.trace(word.word, gems=word_gems)
            t_data = trace_resp.get("trace", trace_resp)

            import csv
            import datetime
            from pathlib import Path

            csv_file = Path(__file__).parent / "game_data/debug_data.csv"
            file_exists = csv_file.exists()

            with open(csv_file, "a", newline="") as f:
                writer = csv.writer(f)
                if not file_exists:
                    # Write the header with every possible variable
                    writer.writerow(
                        [
                            "Timestamp",
                            "Progress",
                            "Word",
                            "Length",
                            "Gems",
                            "Input_Power%",
                            "Input_GemsEnabled",
                            "Input_Boost",
                            "Input_BaseDmgBonus",
                            "Input_EnemyArmour",
                            "LetterPoints",
                            "Math_RawPts",
                            "Math_ClampedPts",
                            "Math_BaseQ",
                            "Math_PowerBonus",
                            "Math_GemSum",
                            "Math_GemBonus",
                            "Math_PreBoost",
                            "Math_PostBoost",
                            "Math_Floored",
                            "ExpectedFinalDmg",
                        ]
                    )

                current_progress = (
                    args.chapter if manual_chapter_override else prog.display()
                )

                writer.writerow(
                    [
                        datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                        current_progress,
                        word.word,
                        len(word.word),
                        word_gems,
                        t_data.get("input_power_percent"),
                        t_data.get("input_gems_enabled"),
                        t_data.get("input_boost_multiplier"),
                        t_data.get("input_base_damage_bonus"),
                        t_data.get("input_enemy_armour"),
                        str(t_data.get("letter_points", [])),
                        t_data.get("raw_points"),
                        t_data.get("clamped_points"),
                        t_data.get("base_q"),
                        t_data.get("power_bonus"),
                        t_data.get("gem_sum"),
                        t_data.get("gem_bonus"),
                        t_data.get("pre_boost_subtotal"),
                        t_data.get("post_boost"),
                        t_data.get("floored"),
                        t_data.get("final_damage"),
                    ]
                )
            print(f"Logged comprehensive trace for '{word.word}' to {csv_file.name}")
        except Exception as e:
            print(f"Failed to trace/log: {e}")
            import traceback

            traceback.print_exc()

        used_powered = bool(word.used_powered)

        try:
            play_word(
                positions,
                box,
                click_delay=click_delay,
                mouse_speed=mouse_move_speed,
                post_word_delay=post_word_delay,
                powered=used_powered,
            )
        except Exception as e:
            overlay.set_status(f"Play failed: {e}")
            import traceback

            traceback.print_exc()
            run_state["auto"] = False
            return

        diamond_count = sum(1 for t in word.tiles if t.gem == "diamond")
        delta = diamond_count - (1 if used_powered else 0)
        if delta != 0:
            overlay.adjust_charges(delta)

        current_hp = overlay.get_threshold()
        if current_hp > 0:
            if word.is_kill:
                overlay.set_hp("")
            else:
                new_hp = max(0, current_hp - word.now)
                overlay.set_hp(str(new_hp) if new_hp > 0 else "")

        if not run_state["auto"]:
            overlay.set_status(f"Played '{word.word}'. Ready.")
            return

        overkill_bit = max(0, word.now - current_hp) if current_hp > 0 else 0
        delay_to_reset = (
            delay_overkill_to_reset
            if (current_hp > 0 and overkill_bit >= 8)
            else delay_kill_to_reset
        )

        overlap_supported = (
            run_state["auto"]
            and not manual_chapter_override
            and rack_persists_into_next(prog)
            and not progress_is_terminal(prog)
        )

        if not overlap_supported:
            overlay.set_status(
                f"Played '{word.word}'. Auto-reset in {delay_to_reset}s..."
            )
            overlay.schedule(int(delay_to_reset * 1000), handle_reset)
            return

        overlay.set_status(
            f"Played '{word.word}'. Pre-reading new rack in {delay_read}s..."
        )

        future_holder: dict = {"future": None, "letters": "", "gems": "", "recs": None}

        def auto_overlap_read():
            try:
                handle_progress_next()
            except Exception as e:
                overlay.set_status(f"Auto progress advance failed: {e}")
                run_state["auto"] = False
                return

            if progress_is_terminal(prog):
                overlay.set_status("Reached final enemy. Halting auto.")
                run_state["auto"] = False
                remaining_ms = max(0, int((delay_to_reset - delay_read) * 1000))
                overlay.schedule(remaining_ms, lambda: _just_reset_no_followup())
                return

            try:
                rack_img = capture_rack(box)
                tiles = split_tiles(rack_img)
            except Exception as e:
                overlay.set_status(f"Pre-read capture failed: {e}")
                run_state["auto"] = False
                return

            if is_rack_paused_or_empty(tiles):
                overlay.set_status("Pre-read: rack paused; halting auto.")
                run_state["auto"] = False
                return

            recs = recognise_rack(tiles, db)
            if any(not r.confident for r in recs):
                overlay.set_status("Pre-read: unknown tiles; halting auto.")
                run_state["auto"] = False
                return

            latest_recs[0] = recs
            overlay.update_rack(recs)

            playable = [r for r in recs if r.status == "normal"]
            letters = "".join(r.letter for r in playable)
            gems = "".join(gem_chars.get(r.gem, " ") for r in playable)
            threshold = overlay.get_threshold()
            charges = overlay.get_charges()

            future_holder["letters"] = letters
            future_holder["gems"] = gems
            future_holder["recs"] = recs
            future_holder["future"] = eng.top_async(
                letters,
                gems=gems,
                n=n_words,
                horizon=horizon,
                max_sims=max_sims,
                threshold=threshold,
                charges=charges,
            )

            overlay.set_status(
                f"Pre-read complete ({letters}). Reset in "
                f"{delay_to_reset - delay_read:.1f}s..."
            )

        def _just_reset_no_followup():
            try:
                if focus_click:
                    focus_game_window(pre_play_delay)
                if ba_window is not None:
                    wl, wt = ba_window.left, ba_window.top
                else:
                    wl, wt = box.left - rack_offset_x, box.top - rack_offset_y
                reset_game(
                    wl,
                    wt,
                    menu_delay=reset_menu_delay,
                    quit_delay=reset_quit_delay,
                    confirm_delay=reset_confirm_delay,
                    mouse_speed=mouse_move_speed,
                )
            except Exception as e:
                overlay.set_status(f"Reset failed: {e}")
            overlay.set_status("Final reset done. Game complete!")

        def auto_overlap_reset_and_play():
            overlay.set_status("Auto: resetting...")
            try:
                if focus_click:
                    focus_game_window(pre_play_delay)
                if ba_window is not None:
                    wl, wt = ba_window.left, ba_window.top
                else:
                    wl, wt = box.left - rack_offset_x, box.top - rack_offset_y
                reset_game(
                    wl,
                    wt,
                    menu_delay=reset_menu_delay,
                    quit_delay=reset_quit_delay,
                    confirm_delay=reset_confirm_delay,
                    mouse_speed=mouse_move_speed,
                )
            except Exception as e:
                overlay.set_status(f"Auto reset failed: {e}")
                run_state["auto"] = False
                return

            if not run_state["auto"]:
                overlay.set_status("Auto: halted before play.")
                return

            wait_ms = max(0, int(delay_post_reset * 1000))
            overlay.schedule(wait_ms, _poll_engine_and_play)

        def _poll_engine_and_play(remaining_polls: int = 200):
            fut = future_holder["future"]
            if fut is None:
                overlay.set_status("Auto: no engine future; halting.")
                run_state["auto"] = False
                return
            if not fut.done():
                if remaining_polls <= 0:
                    overlay.set_status("Auto: engine timeout; halting.")
                    run_state["auto"] = False
                    return
                overlay.set_status("Auto: waiting for engine...")
                overlay.schedule(
                    50, lambda r=remaining_polls - 1: _poll_engine_and_play(r)
                )
                return

            try:
                words = fut.result()
            except Exception as e:
                overlay.set_status(f"Auto: engine error: {e}")
                run_state["auto"] = False
                return

            overlay.update_words(words)
            if not words:
                overlay.set_status("Auto: no words found; halting.")
                run_state["auto"] = False
                return

            top = words[0]
            if not top.is_kill:
                overlay.set_status(
                    "Auto halted: no kill on next enemy (best is {top.word} at {top.now})."
                )
                run_state["auto"] = False
                return

            handle_play(top)

        overlay.schedule(int(delay_read * 1000), auto_overlap_read)
        overlay.schedule(int(delay_to_reset * 1000), auto_overlap_reset_and_play)
        return

    def handle_reset() -> None:
        handle_progress_next()
        overlay.set_status("Focusing window to reset...")
        overlay.root.update_idletasks()
        if focus_click:
            focus_game_window(pre_play_delay)

        overlay.set_status("Executing reset sequence...")
        overlay.root.update_idletasks()

        if ba_window is not None:
            wl, wt = ba_window.left, ba_window.top
        else:
            wl, wt = box.left - rack_offset_x, box.top - rack_offset_y

        try:
            reset_game(
                wl,
                wt,
                menu_delay=reset_menu_delay,
                quit_delay=reset_quit_delay,
                confirm_delay=reset_confirm_delay,
                mouse_speed=mouse_move_speed,
            )
        except Exception as e:
            overlay.set_status(f"Reset sequence failed: {e}")
            import traceback

            traceback.print_exc()
            run_state["auto"] = False
            return

        if run_state["auto"]:
            run_state["auto"] = False
            overlay.set_status("Auto sequence complete. Ready.")
        else:
            overlay.set_status("Reset complete. Ready.")

    def handle_auto() -> None:
        """Trigger the full Read -> Play -> Reset sequence."""
        run_state["auto"] = True
        refresh()

    def refresh() -> None:
        """Trigger just the Read phase."""
        if focus_click:
            overlay.set_status("Focusing game window...")
            overlay.root.update_idletasks()
            focus_game_window(pre_play_delay)
            overlay.schedule(int(focus_settle_delay * 1000), do_capture_and_search)
            return

        delay = max(0.0, focus_delay)
        if delay <= 0:
            do_capture_and_search()
            return

        steps = max(1, int(delay * 10))
        step_ms = int(delay * 1000 / steps)

        def tick(remaining: int) -> None:
            if remaining <= 0:
                do_capture_and_search()
                return
            secs = remaining * step_ms / 1000.0
            overlay.set_status(f"Click the game window... {secs:.1f}s")
            overlay.schedule(step_ms, lambda: tick(remaining - 1))

        tick(steps)

    def autofill_fields_for_current_enemy() -> None:
        hp_hearts, _armour = monster_at(prog)
        if hp_hearts > 0:
            overlay.set_hp(str(hp_hearts * 4))
        else:
            overlay.set_hp("")
        overlay.set_power(power_at(prog))

    autofill_fields_for_current_enemy()

    def handle_progress_next() -> None:
        if manual_chapter_override:
            overlay.set_status("Chapter override in effect; progress disabled.")
            return
        nonlocal prog
        new_prog = progress_advance(prog)
        if new_prog == prog:
            overlay.set_status("Already at the final enemy (3.10). Game complete!")
            return
        prog = new_prog
        save_progress(prog)
        autofill_fields_for_current_enemy()
        push_engine_config_for_progress()
        refresh_progress_display()
        overlay.set_status(f"Advanced to {prog.display()}.")

    def handle_progress_back() -> None:
        if manual_chapter_override:
            overlay.set_status("Chapter override in effect; progress disabled.")
            return
        nonlocal prog
        new_prog = progress_retreat(prog)
        if new_prog == prog:
            overlay.set_status("Already at the first enemy (1.1.1).")
            return
        prog = new_prog
        save_progress(prog)
        autofill_fields_for_current_enemy()
        push_engine_config_for_progress()
        refresh_progress_display()
        overlay.set_status(f"Stepped back to {prog.display()}.")

    def handle_progress_jump(book: int, chapter: int, enemy: int) -> None:
        if manual_chapter_override:
            overlay.set_status("Chapter override in effect; progress disabled.")
            return
        if not progress_is_valid(book, chapter, enemy):
            overlay.set_status(
                f"Jump: {book + 1}.{chapter + 1}.{enemy + 1} is out of range."
            )
            return
        nonlocal prog
        prog = Progress(book=book, chapter=chapter, enemy=enemy)
        save_progress(prog)
        autofill_fields_for_current_enemy()
        push_engine_config_for_progress()
        refresh_progress_display()
        overlay.set_status(f"Jumped to {prog.display()}.")

    def handle_clear() -> None:
        latest_recs[0] = []
        run_state["auto"] = False

    def handle_charges_changed() -> None:
        for lbl in overlay.word_labels:
            lbl.config(text="")
        overlay._current_words = []
        n = overlay.get_charges()
        overlay.set_status(f"⚡ Charges = {n}. Press R to re-score.")

    def handle_power_changed() -> None:
        push_engine_config_for_progress()
        for lbl in overlay.word_labels:
            lbl.config(text="")
        overlay._current_words = []
        overlay.set_status("Power changed. Press R to re-score.")

    overlay.on_refresh(refresh)
    overlay.on_play(handle_play)
    overlay.on_reset(handle_reset)
    overlay.on_auto(handle_auto)
    overlay.on_clear(handle_clear)
    overlay.on_charges_changed(handle_charges_changed)
    overlay.on_progress_next(handle_progress_next)
    overlay.on_progress_back(handle_progress_back)
    overlay.on_progress_jump(handle_progress_jump)
    overlay.on_power_changed(handle_power_changed)

    refresh_progress_display()
    overlay.set_status("Ready. Press R to scan.")

    try:
        overlay.mainloop()
    finally:
        eng.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

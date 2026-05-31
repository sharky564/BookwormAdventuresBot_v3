from __future__ import annotations

import tkinter as tk
from dataclasses import dataclass
from typing import Callable

from engine import ScoredWord
from recognize import ALL_TILE_STATES, Recognition

# Background color per tile state. State is one of the 11
# ALL_TILE_STATES values (normal + 7 gems + 3 afflictions).
STATE_BG_COLORS = {
    "normal": "#2c2c2c",
    "amethyst": "#7b3f8a",
    "emerald": "#2f9f5a",
    "sapphire": "#3a5fcd",
    "garnet": "#ffb343",
    "ruby": "#d62c2c",
    "crystal": "#ffc0cb",
    "diamond": "#d0d0d0",
    # Afflictions: muted reddish/greenish hues that read as "not playable".
    "smashed": "#4a2c2c",
    "locked": "#3a3a3a",
    "plague": "#3a4a2c",
}

# Backwards-compat alias for older code that imports the old name.
GEM_BG_COLORS = STATE_BG_COLORS


@dataclass
class _CorrectionRequest:
    """Carries the data needed to persist a manual tile correction.

    The overlay produces these from its right-click editor; main.py's
    `on_correct` callback consumes them by calling
    `correct_recognition(tile_img, db, letter, state)`. We use a small
    type rather than passing N positional args so the contract is clear.
    """

    tile_img: object  # PIL.Image.Image, kept loose to avoid import here
    letter: str
    state: str


# Public re-export so main.py can do `from overlay import CorrectionRequest`.
CorrectionRequest = _CorrectionRequest


class Overlay:
    WIDTH = 560

    def __init__(self, title: str = "BA Solver", *, geometry: str | None = None):
        self.root = tk.Tk()
        self.root.title(title)
        self.root.attributes("-topmost", True)
        self.root.configure(bg="#1c1c1c")
        self.root.minsize(self.WIDTH, 1)
        self.root.maxsize(self.WIDTH, 10000)
        self.root.resizable(False, True)

        if geometry is None:
            self.root.update_idletasks()
            sw = self.root.winfo_screenwidth()
            self.root.geometry(f"{self.WIDTH}x600+{sw - self.WIDTH - 20}+40")
        else:
            self.root.geometry(geometry)

        self.rack_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.rack_frame.pack(padx=8, pady=8)
        self.tile_labels: list[tk.Label] = []
        for i in range(16):
            lbl = tk.Label(
                self.rack_frame,
                text="?",
                width=2,
                font=("Consolas", 16, "bold"),
                bg="#2c2c2c",
                fg="white",
                borderwidth=1,
                relief="solid",
                cursor="hand2",
            )
            lbl.grid(row=i // 4, column=i % 4, padx=2, pady=2, ipadx=4, ipady=4)
            # Right-click (Button-3) opens the per-tile correction editor.
            # Also bind Control-Click for Mac trackpad users.
            lbl.bind(
                "<Button-3>",
                lambda e, idx=i: self._open_tile_editor(idx, e.x_root, e.y_root),
            )
            lbl.bind(
                "<Control-Button-1>",
                lambda e, idx=i: self._open_tile_editor(idx, e.x_root, e.y_root),
            )
            self.tile_labels.append(lbl)

        self.word_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.word_frame.pack(padx=8, pady=(0, 4), fill="x")

        header = tk.Label(
            self.word_frame,
            text=f"   {'Word':<16}   {'Now':>4}{'Future':>8}{'Total':>8}",
            font=("Consolas", 12, "bold"),
            bg="#1c1c1c",
            fg="#a0a0a0",
        )
        header.pack(anchor="w")

        self.word_labels: list[tk.Label] = []
        for i in range(8):
            lbl = tk.Label(
                self.word_frame,
                text="",
                anchor="w",
                font=("Consolas", 12),
                bg="#1c1c1c",
                fg="white",
                width=46,
                cursor="hand2",
            )
            lbl.pack(anchor="w", fill="x")
            lbl.bind("<Button-1>", lambda e, idx=i: self._play_index(idx))
            lbl.bind("<Enter>", lambda e, w=lbl: w.config(bg="#2a2a2a"))
            lbl.bind("<Leave>", lambda e, w=lbl: w.config(bg="#1c1c1c"))
            self.word_labels.append(lbl)

        self.progress_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.progress_frame.pack(padx=8, pady=(2, 4), fill="x")

        self.progress_var = tk.StringVar(value="—")
        self.progress_label = tk.Label(
            self.progress_frame,
            textvariable=self.progress_var,
            font=("Consolas", 10),
            bg="#1c1c1c",
            fg="#bbb",
            anchor="w",
        )
        self.progress_label.pack(side="left", fill="x", expand=True)

        self.back_btn = tk.Button(
            self.progress_frame,
            text="◀",
            command=lambda: self._progress_back(),
            bg="#333",
            fg="white",
            font=("Helvetica", 9),
            activebackground="#444",
            relief="flat",
            padx=8,
            pady=2,
        )
        self.back_btn.pack(side="left", padx=(4, 0))

        self.next_btn = tk.Button(
            self.progress_frame,
            text="Next ▶",
            command=lambda: self._progress_next(),
            bg="#365",
            fg="white",
            font=("Helvetica", 9, "bold"),
            activebackground="#476",
            relief="flat",
            padx=8,
            pady=2,
        )
        self.next_btn.pack(side="left", padx=(4, 0))

        self.jump_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.jump_frame.pack(padx=8, pady=(0, 4), fill="x")
        tk.Label(
            self.jump_frame,
            text="Jump to:",
            bg="#1c1c1c",
            fg="#bbb",
            font=("Consolas", 10),
        ).pack(side="left")
        self.jump_var = tk.StringVar(value="")
        self.jump_entry = tk.Entry(
            self.jump_frame,
            textvariable=self.jump_var,
            width=10,
            font=("Consolas", 10),
            bg="#2c2c2c",
            fg="white",
            insertbackground="white",
            relief="flat",
        )
        self.jump_entry.pack(side="left", padx=(4, 0))
        self.jump_entry.bind("<Return>", lambda e: self._progress_jump())
        self.jump_btn = tk.Button(
            self.jump_frame,
            text="Go",
            command=lambda: self._progress_jump(),
            bg="#444",
            fg="white",
            font=("Helvetica", 9),
            activebackground="#555",
            relief="flat",
            padx=8,
            pady=2,
        )
        self.jump_btn.pack(side="left", padx=(4, 0))
        tk.Label(
            self.jump_frame,
            text='format "B.C.E", e.g. 2.6.3',
            bg="#1c1c1c",
            fg="#666",
            font=("Consolas", 9),
        ).pack(side="left", padx=(6, 0))

        self.hp_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.hp_frame.pack(padx=8, pady=(2, 4), fill="x")

        tk.Label(
            self.hp_frame,
            text="Enemy HP:",
            bg="#1c1c1c",
            fg="#bbb",
            font=("Consolas", 10),
        ).pack(side="left")
        self.hp_var = tk.StringVar(value="")
        self.hp_entry = tk.Entry(
            self.hp_frame,
            textvariable=self.hp_var,
            width=6,
            font=("Consolas", 10),
            bg="#2c2c2c",
            fg="white",
            insertbackground="white",
            relief="flat",
        )
        self.hp_entry.pack(side="left", padx=(4, 0))

        tk.Label(
            self.hp_frame,
            text="Power%:",
            bg="#1c1c1c",
            fg="#bbb",
            font=("Consolas", 10),
        ).pack(side="left", padx=(12, 0))
        self.power_var = tk.StringVar(value="0.0")
        self._last_power_val = "0.0"
        self.power_entry = tk.Entry(
            self.hp_frame,
            textvariable=self.power_var,
            width=6,
            font=("Consolas", 10),
            bg="#2c2c2c",
            fg="white",
            insertbackground="white",
            relief="flat",
        )
        self.power_entry.pack(side="left", padx=(4, 0))
        self.power_entry.bind("<Return>", lambda e: self._power_changed())
        self.power_entry.bind("<FocusOut>", lambda e: self._power_changed())

        self.charges_var = tk.IntVar(value=0)
        charges_box = tk.Frame(self.hp_frame, bg="#1c1c1c")
        charges_box.pack(side="right", padx=(6, 0))
        self.charges_minus_btn = tk.Button(
            charges_box,
            text="−",
            command=lambda: self._charges_decrement(),
            bg="#333",
            fg="white",
            font=("Helvetica", 9),
            activebackground="#444",
            relief="flat",
            padx=6,
            pady=0,
            width=2,
        )
        self.charges_minus_btn.pack(side="left")
        self.charges_label = tk.Label(
            charges_box,
            text="⚡ 0",
            bg="#1c1c1c",
            fg="#bbb",
            font=("Consolas", 10, "bold"),
            width=5,
            anchor="center",
        )
        self.charges_label.pack(side="left", padx=(2, 2))
        self.charges_plus_btn = tk.Button(
            charges_box,
            text="+",
            command=lambda: self._charges_increment(),
            bg="#333",
            fg="white",
            font=("Helvetica", 9),
            activebackground="#444",
            relief="flat",
            padx=6,
            pady=0,
            width=2,
        )
        self.charges_plus_btn.pack(side="left")

        self.button_frame = tk.Frame(self.root, bg="#1c1c1c")
        self.button_frame.pack(padx=8, pady=(2, 6), fill="x")

        self.read_btn = tk.Button(
            self.button_frame,
            text="👁 Read (R)",
            command=lambda: self._refresh(),
            bg="#444",
            fg="white",
            font=("Helvetica", 10),
            activebackground="#555",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.read_btn.pack(side="left")

        self.play_btn = tk.Button(
            self.button_frame,
            text="▶️ Play",
            command=lambda: self._play_index(0),
            bg="#3a7",
            fg="white",
            font=("Helvetica", 10, "bold"),
            activebackground="#4b8",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.play_btn.pack(side="left", padx=(6, 0))

        self.reset_btn = tk.Button(
            self.button_frame,
            text="↺ Reset",
            command=lambda: self._reset(),
            bg="#833",
            fg="white",
            font=("Helvetica", 10),
            activebackground="#a44",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.reset_btn.pack(side="left", padx=(6, 0))

        self.clear_btn = tk.Button(
            self.button_frame,
            text="✕ Clear",
            command=lambda: self._clear(),
            bg="#444",
            fg="#bbb",
            font=("Helvetica", 10),
            activebackground="#555",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.clear_btn.pack(side="left", padx=(6, 0))

        self.auto_btn = tk.Button(
            self.button_frame,
            text="⚡ Auto",
            command=lambda: self._auto(),
            bg="#883",
            fg="white",
            font=("Helvetica", 10, "bold"),
            activebackground="#aa4",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.auto_btn.pack(side="left", padx=(6, 0))

        self.review_btn = tk.Button(
            self.button_frame,
            text="✏️ Review",
            command=lambda: self._review(),
            bg="#444",
            fg="white",
            font=("Helvetica", 10),
            activebackground="#555",
            relief="flat",
            padx=10,
            pady=4,
        )
        self.review_btn.pack(side="left", padx=(6, 0))

        self.status_var = tk.StringVar(
            value="Press R to scan, click a word to play it."
        )
        self.status = tk.Label(
            self.root,
            textvariable=self.status_var,
            font=("Consolas", 10),
            bg="#1c1c1c",
            fg="#808080",
            wraplength=self.WIDTH - 20,
            justify="left",
            anchor="w",
        )
        self.status.pack(pady=(0, 6), padx=10, fill="x")

        self._refresh_callback = None
        self._play_callback = None
        self._reset_callback = None
        self._auto_callback = None
        self._clear_callback = None
        self._charges_changed_callback = None
        self._progress_next_callback = None
        self._progress_back_callback = None
        self._progress_jump_callback = None
        # Called when the user manually corrects a tile via right-click
        # editor or the Review button. Signature: fn(index, new_recognition).
        # main.py wires this up; the overlay refreshes itself afterwards.
        self._correct_callback: Callable[[int, Recognition], None] | None = None
        self._review_callback: Callable[[], None] | None = None
        # Most recent recognitions + tile images, set by update_rack. The
        # right-click correction editor needs the source image to write
        # back to the bank via correct_recognition.
        self._latest_recs: list[Recognition] = []
        self._latest_tiles: list | None = None
        self._current_words: list[ScoredWord] = []

        def _if_not_in_entry(fn):
            def wrapper(e):
                if isinstance(self.root.focus_get(), tk.Entry):
                    return
                fn()

            return wrapper

        self.root.bind("<r>", _if_not_in_entry(self._refresh))
        self.root.bind("<R>", _if_not_in_entry(self._refresh))
        self.root.bind("<q>", _if_not_in_entry(self.root.destroy))
        self.root.bind("<Q>", _if_not_in_entry(self.root.destroy))

    def on_refresh(self, fn) -> None:
        self._refresh_callback = fn

    def on_play(self, fn) -> None:
        self._play_callback = fn

    def on_reset(self, fn) -> None:
        self._reset_callback = fn

    def on_auto(self, fn) -> None:
        self._auto_callback = fn

    def on_clear(self, fn) -> None:
        self._clear_callback = fn

    def on_power_changed(self, fn) -> None:
        self._power_changed_callback = fn

    def on_charges_changed(self, fn) -> None:
        self._charges_changed_callback = fn

    def on_progress_next(self, fn) -> None:
        self._progress_next_callback = fn

    def on_progress_back(self, fn) -> None:
        self._progress_back_callback = fn

    def on_progress_jump(self, fn) -> None:
        self._progress_jump_callback = fn

    def on_correct(self, fn: Callable[[int, Recognition], None]) -> None:
        """Called when the user manually corrects a single tile.

        `fn(index, new_recognition)` should update the caller's state
        (e.g. `recs[index] = new_recognition`) and refresh anything
        downstream. The overlay also refreshes its own display.
        """
        self._correct_callback = fn

    def on_review(self, fn: Callable[[], None]) -> None:
        """Called when the user clicks the Review button.

        `fn()` should walk all 16 tiles in the current rack (typically
        by calling `review_rack_interactively`) and update its state
        accordingly. The overlay does not refresh itself in response —
        the callback is expected to call `update_rack` once done.
        """
        self._review_callback = fn

    def _refresh(self) -> None:
        if self._refresh_callback:
            self._refresh_callback()

    def _reset(self) -> None:
        if self._reset_callback:
            self._reset_callback()

    def _auto(self) -> None:
        if self._auto_callback:
            self._auto_callback()

    def _review(self) -> None:
        if self._review_callback:
            self._review_callback()
        else:
            self.set_status("Review not wired up.")

    def _open_tile_editor(self, index: int, screen_x: int, screen_y: int) -> None:
        """Open a small popup to edit a single tile's letter and state.

        Triggered by right-click (or Control-Click) on a tile label. On
        commit, calls the registered `on_correct` callback with the new
        Recognition, then refreshes the rack display.

        Requires that update_rack was called with a `tiles` argument
        (so we have the source image to write back to the bank).
        """
        if index < 0 or index >= len(self._latest_recs):
            return
        if self._latest_tiles is None or index >= len(self._latest_tiles):
            self.set_status("Tile image unavailable — read the rack first (R).")
            return
        if self._correct_callback is None:
            self.set_status("Correction not wired up.")
            return

        rec = self._latest_recs[index]
        tile_img = self._latest_tiles[index]

        win = tk.Toplevel(self.root)
        win.title(f"Tile {index}")
        win.attributes("-topmost", True)
        win.transient(self.root)
        win.configure(bg="#1c1c1c")
        # Position near the cursor without spilling off-screen.
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        x = min(max(0, screen_x), sw - 240)
        y = min(max(0, screen_y), sh - 180)
        win.geometry(f"+{x}+{y}")
        # Modal-lite: grab focus but don't block the whole app.
        win.grab_set()

        info = tk.Label(
            win,
            text=(
                f"Tile {index}: detected '{rec.letter or '?'}' / {rec.state} "
                f"(dist {rec.distance:.1f})"
            ),
            bg="#1c1c1c",
            fg="#bbb",
            font=("Consolas", 9),
        )
        info.pack(padx=10, pady=(8, 4))

        # Letter field
        letter_frame = tk.Frame(win, bg="#1c1c1c")
        letter_frame.pack(pady=2)
        tk.Label(
            letter_frame, text="Letter:", bg="#1c1c1c", fg="white"
        ).pack(side="left", padx=(0, 4))
        letter_var = tk.StringVar(value=rec.letter)
        letter_entry = tk.Entry(
            letter_frame,
            textvariable=letter_var,
            width=4,
            font=("Helvetica", 14),
            bg="#2c2c2c",
            fg="white",
            insertbackground="white",
            relief="flat",
        )
        letter_entry.pack(side="left")

        # State dropdown
        state_frame = tk.Frame(win, bg="#1c1c1c")
        state_frame.pack(pady=2)
        tk.Label(
            state_frame, text="State: ", bg="#1c1c1c", fg="white"
        ).pack(side="left", padx=(0, 4))
        state_var = tk.StringVar(value=rec.state)
        state_menu = tk.OptionMenu(state_frame, state_var, *ALL_TILE_STATES)
        state_menu.config(bg="#2c2c2c", fg="white", activebackground="#3c3c3c", width=10)
        state_menu["menu"].config(bg="#2c2c2c", fg="white")
        state_menu.pack(side="left")

        tk.Label(
            win,
            text="Enter = save   Esc = cancel",
            bg="#1c1c1c",
            fg="#666",
            font=("Consolas", 9),
        ).pack(pady=(6, 8))

        def commit(event=None):
            letter = letter_var.get().strip()
            state = state_var.get().strip()
            if letter and letter[0].isalpha() and state in ALL_TILE_STATES:
                letter = letter.upper() if len(letter) == 1 else letter
                # Only persist & propagate when the user actually changed
                # something (otherwise this is just a confirmation).
                if letter != rec.letter or state != rec.state:
                    # Build the correction request; the callback (which
                    # has the db reference) is responsible for calling
                    # correct_recognition() with it.
                    self._correct_callback(index, _CorrectionRequest(
                        tile_img=tile_img,
                        letter=letter,
                        state=state,
                    ))
            win.destroy()

        def cancel(event=None):
            win.destroy()

        letter_entry.bind("<Return>", commit)
        win.bind("<Return>", commit)
        win.bind("<Escape>", cancel)
        win.protocol("WM_DELETE_WINDOW", cancel)
        letter_entry.focus_set()
        letter_entry.select_range(0, "end")

    def _clear(self) -> None:
        self.clear_state()
        if self._clear_callback:
            self._clear_callback()

    def _refresh_charges_label(self) -> None:
        n = self.charges_var.get()
        self.charges_label.config(
            text=f"⚡ {n}",
            fg=("#bbb" if n == 0 else "#ffeb70"),
        )
        self.charges_minus_btn.config(state="disabled" if n <= 0 else "normal")

    def _charges_increment(self, fire_callback: bool = True) -> None:
        self.charges_var.set(self.charges_var.get() + 1)
        self._refresh_charges_label()
        if fire_callback and self._charges_changed_callback:
            self._charges_changed_callback()

    def _charges_decrement(self, fire_callback: bool = True) -> None:
        n = self.charges_var.get()
        if n <= 0:
            return
        self.charges_var.set(n - 1)
        self._refresh_charges_label()
        if fire_callback and self._charges_changed_callback:
            self._charges_changed_callback()

    def _progress_next(self) -> None:
        if self._progress_next_callback:
            self._progress_next_callback()

    def _progress_back(self) -> None:
        if self._progress_back_callback:
            self._progress_back_callback()

    def _progress_jump(self) -> None:
        raw = self.jump_var.get().strip()
        parts = raw.split(".")
        if len(parts) != 3:
            self.set_status(f"Jump: expected B.C.E (e.g. 2.6.3), got '{raw}'.")
            return
        try:
            book = int(parts[0]) - 1
            chapter = int(parts[1]) - 1
            enemy = int(parts[2]) - 1
        except ValueError:
            self.set_status(f"Jump: expected integers in B.C.E, got '{raw}'.")
            return
        if self._progress_jump_callback:
            self._progress_jump_callback(book, chapter, enemy)
            self.jump_var.set("")

    def _power_changed(self) -> None:
        current = self.power_var.get().strip()
        if current != self._last_power_val:
            self._last_power_val = current
            if self._power_changed_callback:
                self._power_changed_callback()

    def get_power(self) -> float:
        try:
            return float(self.power_var.get().strip())
        except ValueError:
            return 0.0

    def set_power(self, power: float) -> None:
        val_str = f"{power:.1f}"
        self.power_var.set(val_str)
        self._last_power_val = val_str

    def update_progress(self, label: str, *, at_end: bool = False) -> None:
        """Update the progress label. If at_end, disable the Next button."""
        self.progress_var.set(label)
        self.next_btn.config(state="disabled" if at_end else "normal")

    def _play_index(self, idx: int) -> None:
        if not (0 <= idx < len(self._current_words)):
            return
        if self._play_callback is None:
            self.set_status("Play not wired up.")
            return
        self._play_callback(self._current_words[idx])

    def set_status(self, text: str) -> None:
        self.status_var.set(text)

    def update_rack(
        self,
        recognitions: list[Recognition],
        tiles: list | None = None,
    ) -> None:
        self._latest_recs = list(recognitions)
        self._latest_tiles = list(tiles) if tiles is not None else None

        for i, rec in enumerate(recognitions):
            text = rec.letter if rec.letter else "?"
            bg = STATE_BG_COLORS.get(rec.state, "#2c2c2c")
            # Pick foreground for legibility against the bg.
            if rec.state == "diamond":
                fg = "black"
            elif rec.state == "crystal":
                fg = "#222"
            else:
                fg = "white"
            if not rec.confident:
                fg = "#ff5050"
            self.tile_labels[i].config(text=text, bg=bg, fg=fg)

    def update_words(self, words: list[ScoredWord]) -> None:
        self._current_words = list(words)
        for i, lbl in enumerate(self.word_labels):
            if i < len(words):
                w = words[i]
                kill_marker = "💀 " if w.is_kill else "   "
                pow_marker = "💪 " if w.used_powered else "   "
                lbl.config(
                    text=f"{kill_marker}{w.word:<16}{pow_marker}{w.now:>4}{w.future:>8.2f}{w.total:>8.2f}",
                    fg=("#ff7" if w.is_kill else ("white" if i > 0 else "#5fdf5f")),
                )
            else:
                lbl.config(text="")

    def clear_state(self) -> None:
        for lbl in self.tile_labels:
            lbl.config(text="?", bg="#2c2c2c", fg="white")
        for lbl in self.word_labels:
            lbl.config(text="")
        self._current_words = []
        self.set_status("Cleared. Press R to scan.")

    def get_threshold(self) -> int:
        raw = self.hp_var.get().strip()
        if not raw:
            return 0
        try:
            v = int(raw)
            return v if v > 0 else 0
        except ValueError:
            return 0

    def set_hp(self, hp_str: str) -> None:
        self.hp_var.set(hp_str)

    def get_charges(self) -> int:
        return max(0, int(self.charges_var.get()))

    def set_charges(self, value: int, *, fire_callback: bool = False) -> None:
        self.charges_var.set(max(0, int(value)))
        self._refresh_charges_label()
        if fire_callback and self._charges_changed_callback:
            self._charges_changed_callback()

    def adjust_charges(self, delta: int, *, fire_callback: bool = False) -> None:
        self.set_charges(self.get_charges() + delta, fire_callback=fire_callback)

    def schedule(self, ms: int, fn) -> None:
        self.root.after(ms, fn)

    def mainloop(self) -> None:
        self.root.mainloop()
from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from pathlib import Path

from PIL import Image
import mss


_SCRIPT_DIR = Path(__file__).resolve().parent
CONFIG_PATH = _SCRIPT_DIR / "wordgame_calibration.json"

DEFAULT_RACK_OFFSET_X = 304
DEFAULT_RACK_OFFSET_Y = 335
DEFAULT_TILE_SIZE_X = 50
DEFAULT_TILE_SIZE_Y = 51

WINDOW_TITLE_PATTERNS = ["Bookworm Adventures Deluxe"]


@dataclass
class RackBox:
    left: int
    top: int
    right: int
    bottom: int

    @property
    def width(self) -> int:
        return self.right - self.left + 1

    @property
    def height(self) -> int:
        return self.bottom - self.top + 1

    def tile_bbox(self, row: int, col: int) -> tuple[int, int, int, int]:
        w_per = self.width / 4
        h_per = self.height / 4
        l = int(self.left + col * w_per)
        t = int(self.top + row * h_per)
        r = int(self.left + (col + 1) * w_per) - 1
        b = int(self.top + (row + 1) * h_per) - 1
        return l, t, r, b


def load_calibration(path: Path = CONFIG_PATH) -> RackBox | None:
    if not path.exists():
        return None
    try:
        d = json.loads(path.read_text())
        return RackBox(**d)
    except Exception:
        return None


def save_calibration(box: RackBox, path: Path = CONFIG_PATH) -> None:
    path.write_text(json.dumps(asdict(box), indent=2))


def capture_screen() -> Image.Image:
    with mss.mss() as sct:
        mon = sct.monitors[1]
        shot = sct.grab(mon)
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def capture_rack(box: RackBox) -> Image.Image:
    region = {
        "left": box.left,
        "top": box.top,
        "width": box.width,
        "height": box.height,
    }
    with mss.mss() as sct:
        shot = sct.grab(region)
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def split_tiles(rack_img: Image.Image, inset: float = 0.10) -> list[Image.Image]:
    w, h = rack_img.size
    tw, th = w / 4, h / 4
    inset_x, inset_y = int(tw * inset), int(th * inset)
    tiles = []
    for row in range(4):
        for col in range(4):
            left = int(col * tw) + inset_x
            top = int(row * th) + inset_y
            right = int((col + 1) * tw) - inset_x
            bottom = int((row + 1) * th) - inset_y
            tiles.append(rack_img.crop((left, top, right, bottom)))
    return tiles


def find_game_window(title_patterns: list[str] | None = None):
    import pygetwindow as gw

    patterns = title_patterns or WINDOW_TITLE_PATTERNS

    for title in patterns:
        wins = gw.getWindowsWithTitle(title)
        if wins:
            return wins[0]

    all_wins = gw.getAllWindows()
    for w in all_wins:
        wt = (w.title or "").lower()
        for pat in patterns:
            if pat.lower() in wt:
                return w
    return None


def rack_box_from_window(
    window,
    rack_offset_x: int = DEFAULT_RACK_OFFSET_X,
    rack_offset_y: int = DEFAULT_RACK_OFFSET_Y,
    tile_size_x: int = DEFAULT_TILE_SIZE_X,
    tile_size_y: int = DEFAULT_TILE_SIZE_Y,
) -> RackBox:
    left = window.left + rack_offset_x
    top = window.top + rack_offset_y
    right = left + tile_size_x * 4 - 1
    bottom = top + tile_size_y * 4 - 1
    return RackBox(left=left, top=top, right=right, bottom=bottom)


def auto_detect_rack(
    rack_offset_x: int = DEFAULT_RACK_OFFSET_X,
    rack_offset_y: int = DEFAULT_RACK_OFFSET_Y,
    tile_size_x: int = DEFAULT_TILE_SIZE_X,
    tile_size_y: int = DEFAULT_TILE_SIZE_Y,
    title_patterns: list[str] | None = None,
) -> RackBox | None:
    win = find_game_window(title_patterns)
    if win is None:
        return None
    print(
        f"Found game window '{win.title}' at ({win.left}, {win.top}) size {win.width}x{win.height}",
        flush=True,
    )
    return rack_box_from_window(
        win, rack_offset_x, rack_offset_y, tile_size_x, tile_size_y
    )


def run_calibration(parent=None, focus_delay: float = 3.0) -> RackBox:
    import tkinter as tk
    import time

    if focus_delay > 0:
        print(
            f"Calibration: focus the game window, capturing in {focus_delay:.1f}s...",
            flush=True,
        )
        time.sleep(focus_delay)

    screen = capture_screen()
    print(f"Captured screen: {screen.size}. Opening calibration window...", flush=True)

    owns_root = parent is None
    if owns_root:
        parent = tk.Tk()
        parent.withdraw()

    sw = parent.winfo_screenwidth()
    sh = parent.winfo_screenheight()

    root = tk.Toplevel(parent)
    root.title("wordgame calibration")
    root.geometry(f"{sw}x{sh}+0+0")
    root.overrideredirect(True)
    root.attributes("-topmost", True)

    canvas = tk.Canvas(
        root, width=sw, height=sh, highlightthickness=0, bg="black", cursor="crosshair"
    )
    canvas.pack(fill="both", expand=True)

    from PIL import ImageTk

    tk_img = ImageTk.PhotoImage(screen.resize((sw, sh)), master=root)
    canvas.create_image(0, 0, image=tk_img, anchor="nw")
    canvas.image = tk_img

    canvas.create_rectangle(0, 0, sw, 80, fill="black", outline="")
    text = canvas.create_text(
        sw // 2,
        40,
        text="Click the CENTRE of the TOP-LEFT tile (row 0, col 0) - Esc to cancel",
        fill="yellow",
        font=("Helvetica", 18, "bold"),
    )

    root.update_idletasks()
    root.update()
    root.lift()
    root.focus_force()
    root.after(50, root.focus_force)

    clicks: list[tuple[int, int]] = []
    cancelled = {"v": False}

    def on_click(event):
        sx = int(event.x * screen.size[0] / sw)
        sy = int(event.y * screen.size[1] / sh)
        clicks.append((sx, sy))
        r = 8
        canvas.create_oval(
            event.x - r, event.y - r, event.x + r, event.y + r, outline="red", width=3
        )
        if len(clicks) == 1:
            canvas.itemconfig(
                text, text="Click the CENTER of the BOTTOM-RIGHT tile (row 3, col 3)"
            )
        else:
            root.after(300, root.destroy)

    def on_escape(event=None):
        cancelled["v"] = True
        root.destroy()

    canvas.bind("<Button-1>", on_click)
    root.bind("<Escape>", on_escape)
    root.bind("<Key-q>", on_escape)
    root.protocol("WM_DELETE_WINDOW", on_escape)

    parent.wait_window(root)

    if owns_root:
        parent.destroy()

    if cancelled["v"] or len(clicks) != 2:
        raise RuntimeError("Calibration cancelled (Esc/Q pressed)")

    (cx0, cy0), (cx1, cy1) = clicks
    width = (cx1 - cx0) * 4 / 3
    height = (cy1 - cy0) * 4 / 3
    left = cx0 - width / 8
    top = cy0 - height / 8
    box = RackBox(
        left=int(round(left)),
        top=int(round(top)),
        right=int(round(left + width - 1)),
        bottom=int(round(top + height - 1)),
    )
    save_calibration(box)
    return box


def ensure_calibration(
    force: bool = False, parent=None, focus_delay: float = 3.0
) -> RackBox:
    if not force:
        box = load_calibration()
        if box is not None:
            return box
    return run_calibration(parent=parent, focus_delay=focus_delay)


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--manual-calibrate",
        action="store_true",
        help="Use the click-based Tk calibration UI",
    )
    ap.add_argument("--rack-offset-x", type=int, default=DEFAULT_RACK_OFFSET_X)
    ap.add_argument("--rack-offset-y", type=int, default=DEFAULT_RACK_OFFSET_Y)
    ap.add_argument("--tile-size-x", type=int, default=DEFAULT_TILE_SIZE_X)
    ap.add_argument("--tile-size-y", type=int, default=DEFAULT_TILE_SIZE_Y)
    ap.add_argument(
        "--save-tiles",
        metavar="DIR",
        help="Capture the rack and save 16 tile crops to DIR",
    )
    args = ap.parse_args()

    box = None
    if not args.manual_calibrate:
        box = auto_detect_rack(
            args.rack_offset_x, args.rack_offset_y, args.tile_size_x, args.tile_size_y
        )
    if box is None:
        box = ensure_calibration(force=True)

    print(
        f"Rack box: ({box.left},{box.top}) -> ({box.right},{box.bottom}) size={box.width}x{box.height}"
    )

    if args.save_tiles:
        outdir = Path(args.save_tiles)
        outdir.mkdir(parents=True, exist_ok=True)
        import time

        time.sleep(3)
        rack = capture_rack(box)
        rack.save(outdir / "rack.png")
        for i, tile in enumerate(split_tiles(rack)):
            tile.save(outdir / f"tile_{i:02d}.png")
        print(f"Saved rack and 16 tile crops to {outdir}")

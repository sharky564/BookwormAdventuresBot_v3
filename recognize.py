from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from pathlib import Path

import numpy as np
from PIL import Image


_SCRIPT_DIR = Path(__file__).resolve().parent
TILE_DB_PATH = _SCRIPT_DIR / "wordgame_tiles.json"


def is_rack_paused_or_empty(
    tiles: list[Image.Image], min_brightness: float = 0.25, max_brightness: float = 0.92
) -> bool:
    arr_means = []
    arr_stds = []
    for t in tiles:
        a = np.asarray(t.convert("L"), dtype=np.float32) / 255.0
        arr_means.append(float(a.mean()))
        arr_stds.append(float(a.std()))

    avg_mean = sum(arr_means) / len(arr_means)
    avg_std = sum(arr_stds) / len(arr_stds)

    if avg_mean < min_brightness and avg_std < 0.10:
        return True
    if avg_mean > max_brightness and avg_std < 0.08:
        return True
    if avg_std < 0.04:
        return True
    return False


_CENTROIDS_PATH = Path(__file__).resolve().parent / "gem_centroids.json"
GEM_DISTANCE_THRESHOLD = 50


def _load_centroids() -> dict[str, tuple[int, int, int]]:
    if not _CENTROIDS_PATH.exists():
        return {
            "none": (240, 220, 155),
            "amethyst": (215, 145, 245),
            "emerald": (70, 185, 80),
            "sapphire": (130, 180, 245),
            "garnet": (250, 145, 50),
            "ruby": (242, 130, 130),
            "crystal": (240, 200, 245),
            "diamond": (250, 250, 250),
        }
    raw = json.loads(_CENTROIDS_PATH.read_text())
    return {name: (c["r"], c["g"], c["b"]) for name, c in raw["centroids"].items()}


GEM_CENTROIDS = _load_centroids()
HASH_SIZE = 16
LETTER_DARK_THRESHOLD = 90


def phash(img: Image.Image) -> int:
    small = img.convert("L").resize((HASH_SIZE, HASH_SIZE), Image.LANCZOS)
    arr = np.asarray(small, dtype=np.uint8)
    bits = (arr < LETTER_DARK_THRESHOLD).flatten()
    h_val = 0
    for b in bits:
        h_val = (h_val << 1) | int(b)
    return h_val


def hash_hex(h: int) -> str:
    return f"{h:0{(HASH_SIZE * HASH_SIZE) // 4}x}"


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


SMASHED_SAT_MAX = 0.275
SMASHED_VAL_RANGE = (0.20, 0.80)
PLAGUE_GREEN_HUE_RANGE = (90, 160)
PLAGUE_GREEN_SAT_MIN = 0.40
PLAGUE_GREEN_VAL_MIN = 0.35
PLAGUE_PIXEL_FRACTION_MIN = 0.05
PLAGUE_SPREAD_FRAC_MIN = 0.15
LOCKED_CORNER_FRAC = 0.22
LOCKED_DARK_THRESHOLD = 110
LOCKED_CORNER_DARK_FRAC = 0.20
WILDCARD_VARIANCE_MIN = 6000


def _corner_medians(arr: np.ndarray) -> list[np.ndarray]:
    h, w, _ = arr.shape
    cx_lo = max(3, int(w * 0.15))
    cx_hi = max(8, int(w * 0.28))
    cy_lo = max(3, int(h * 0.15))
    cy_hi = max(8, int(h * 0.28))
    patches = [
        arr[cy_lo:cy_hi, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[cy_lo:cy_hi, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
    ]
    return [np.median(p, axis=0) if p.size > 0 else np.zeros(3) for p in patches]


def _is_wildcard(corner_meds: list[np.ndarray]) -> bool:
    if not corner_meds:
        return False
    stacked = np.stack(corner_meds)
    variance = float(np.sum(np.var(stacked, axis=0)))
    return variance >= WILDCARD_VARIANCE_MIN


def detect_status(tile_img: Image.Image) -> str:
    arr_norm = np.asarray(tile_img.convert("RGB"), dtype=np.float32) / 255.0
    arr_255 = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    h, w, _ = arr_norm.shape

    corner_meds = _corner_medians(arr_255)
    if _is_wildcard(corner_meds):
        return "normal"

    if detect_gem(tile_img) != "none":
        return "normal"

    smashed_pixels = _sample_corner_pixels(tile_img).astype(np.float32) / 255.0
    cmax = smashed_pixels.max(axis=1)
    delta = cmax - smashed_pixels.min(axis=1)
    with np.errstate(divide="ignore", invalid="ignore"):
        sat = np.where(cmax > 0, delta / cmax, 0)
    val = cmax
    body_mask = val > 0.20
    if body_mask.sum() >= 10:
        median_sat = float(np.median(sat[body_mask]))
        median_val = float(np.median(val[body_mask]))
        if (
            median_sat <= SMASHED_SAT_MAX
            and SMASHED_VAL_RANGE[0] <= median_val <= SMASHED_VAL_RANGE[1]
        ):
            return "smashed"

    full2d = arr_norm
    fr2 = full2d[:, :, 0]
    fg2 = full2d[:, :, 1]
    fb2 = full2d[:, :, 2]
    cmax2 = full2d.max(axis=2)
    delta2 = cmax2 - full2d.min(axis=2)

    with np.errstate(divide="ignore", invalid="ignore"):
        hue2 = np.zeros_like(cmax2)
        m2 = delta2 > 0
        rm2 = m2 & (cmax2 == fr2)
        hue2[rm2] = ((fg2[rm2] - fb2[rm2]) / delta2[rm2]) % 6
        gm2 = m2 & (cmax2 == fg2) & ~rm2
        hue2[gm2] = ((fb2[gm2] - fr2[gm2]) / delta2[gm2]) + 2
        bm2 = m2 & ~rm2 & ~gm2
        hue2[bm2] = ((fr2[bm2] - fg2[bm2]) / delta2[bm2]) + 4
        hue2 = (hue2 * 60) % 360
        sat2 = np.where(cmax2 > 0, delta2 / cmax2, 0)
    val2 = cmax2

    green_mask_2d = (
        (PLAGUE_GREEN_HUE_RANGE[0] <= hue2)
        & (hue2 <= PLAGUE_GREEN_HUE_RANGE[1])
        & (sat2 >= PLAGUE_GREEN_SAT_MIN)
        & (val2 >= PLAGUE_GREEN_VAL_MIN)
    )
    total_green = int(green_mask_2d.sum())
    total_px = green_mask_2d.size

    if total_green / total_px >= PLAGUE_PIXEL_FRACTION_MIN:
        ys, xs = np.where(green_mask_2d)
        if len(xs) >= 3:
            std_x = float(np.std(xs))
            std_y = float(np.std(ys))
            spread_ok = (
                std_x >= w * PLAGUE_SPREAD_FRAC_MIN
                and std_y >= h * PLAGUE_SPREAD_FRAC_MIN
            )
            if spread_ok:
                eroded = green_mask_2d.copy()
                for _ in range(2):
                    up = np.zeros_like(eroded)
                    up[1:] = eroded[:-1]
                    dn = np.zeros_like(eroded)
                    dn[:-1] = eroded[1:]
                    lf = np.zeros_like(eroded)
                    lf[:, 1:] = eroded[:, :-1]
                    rt = np.zeros_like(eroded)
                    rt[:, :-1] = eroded[:, 1:]
                    eroded = eroded & up & dn & lf & rt
                shrinkage = 1.0 - eroded.sum() / max(1, total_green)
                if shrinkage > 0.60:
                    return "plague"

    gray = np.asarray(tile_img.convert("L"), dtype=np.uint8)
    cw = int(w * LOCKED_CORNER_FRAC)
    ch = int(h * LOCKED_CORNER_FRAC)
    dark_mask = gray < LOCKED_DARK_THRESHOLD
    corner_masks = [
        dark_mask[:ch, :cw],
        dark_mask[:ch, w - cw :],
        dark_mask[h - ch :, :cw],
        dark_mask[h - ch :, w - cw :],
    ]
    dark_fracs = sorted([m.sum() / max(1, m.size) for m in corner_masks], reverse=True)
    if dark_fracs[2] >= LOCKED_CORNER_DARK_FRAC:
        return "locked"

    return "normal"


def _sample_corner_pixels(tile_img: Image.Image) -> np.ndarray:
    arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    h, w, _ = arr.shape
    cx_lo = max(3, int(w * 0.15))
    cx_hi = max(8, int(w * 0.28))
    cy_lo = max(3, int(h * 0.15))
    cy_hi = max(8, int(h * 0.28))
    patches = [
        arr[cy_lo:cy_hi, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[cy_lo:cy_hi, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
    ]
    return np.concatenate(patches, axis=0)


def _rgb_distance(
    rgb_a: tuple[float, float, float], rgb_b: tuple[float, float, float]
) -> float:
    return float(
        np.sqrt(
            (rgb_a[0] - rgb_b[0]) ** 2
            + (rgb_a[1] - rgb_b[1]) ** 2
            + (rgb_a[2] - rgb_b[2]) ** 2
        )
    )


def detect_gem(tile_img: Image.Image) -> str:
    arr_255 = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    corner_meds = _corner_medians(arr_255)
    if _is_wildcard(corner_meds):
        return "none"

    pixels = _sample_corner_pixels(tile_img)
    if pixels.size == 0:
        return "none"
    median_rgb = np.median(pixels, axis=0)

    best_name = "none"
    best_dist = float("inf")
    for name, centroid in GEM_CENTROIDS.items():
        d = _rgb_distance(median_rgb, centroid)
        if d < best_dist:
            best_dist = d
            best_name = name

    if best_dist > GEM_DISTANCE_THRESHOLD:
        return "none"
    return best_name


@dataclass
class TileEntry:
    hash: str
    letter: str
    gem: str


class TileDB:
    DB_SCHEMA = 4

    def __init__(self, path: Path = TILE_DB_PATH):
        self.path = path
        self.entries: list[TileEntry] = []
        if path.exists():
            try:
                raw = json.loads(path.read_text())
                if isinstance(raw, dict):
                    schema = int(raw.get("schema", 1))
                    if schema == self.DB_SCHEMA:
                        self.entries = [TileEntry(**e) for e in raw.get("entries", [])]
                    else:
                        backup = path.with_suffix(f".v{schema}.bak.json")
                        path.rename(backup)
                        print(
                            f"Tile DB schema v{schema} found; archived to "
                            f"{backup.name}. Starting fresh."
                        )
                else:
                    backup = path.with_suffix(".v1.bak.json")
                    path.rename(backup)
                    print(
                        f"Tile DB v1 (pre-letter-isolation) found; archived to "
                        f"{backup.name}. Re-label tiles when prompted."
                    )
            except Exception as e:
                print(f"Tile DB load failed ({e}); starting fresh.")
        self._by_hash: dict[int, TileEntry] = {int(e.hash, 16): e for e in self.entries}

    def save(self) -> None:
        payload = {
            "schema": self.DB_SCHEMA,
            "entries": [asdict(e) for e in self.entries],
        }
        self.path.write_text(json.dumps(payload, indent=2))

    def add(self, h: int, letter: str, gem: str = "none") -> None:
        ent = TileEntry(hash=hash_hex(h), letter=letter.upper(), gem=gem)
        self._by_hash[h] = ent
        self.entries = [e for e in self.entries if int(e.hash, 16) != h]
        self.entries.append(ent)
        self.save()

    def lookup(self, h: int, max_distance: int | None = None) -> TileEntry | None:
        if max_distance is None:
            max_distance = max(2, (HASH_SIZE * HASH_SIZE) // 22)

        if h in self._by_hash:
            return self._by_hash[h]
        best: TileEntry | None = None
        best_dist = max_distance + 1
        for e in self.entries:
            d = hamming(h, int(e.hash, 16))
            if d < best_dist:
                best_dist = d
                best = e
        return best

    def __len__(self) -> int:
        return len(self.entries)


TILE_STATUSES = ("normal", "smashed", "locked", "plague")


@dataclass
class Recognition:
    letter: str
    gem: str
    hash: int
    distance: int
    confident: bool
    status: str = "normal"


def inspect_tile(tile_img: Image.Image) -> dict:
    arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    h, w, _ = arr.shape
    cx_lo = max(3, int(w * 0.15))
    cx_hi = max(8, int(w * 0.28))
    cy_lo = max(3, int(h * 0.15))
    cy_hi = max(8, int(h * 0.28))

    corner_samples = [
        ("TL", arr[cy_lo:cy_hi, cx_lo:cx_hi, :]),
        ("TR", arr[cy_lo:cy_hi, w - cx_hi : w - cx_lo, :]),
        ("BL", arr[h - cy_hi : h - cy_lo, cx_lo:cx_hi, :]),
        ("BR", arr[h - cy_hi : h - cy_lo, w - cx_hi : w - cx_lo, :]),
    ]

    corners_detail: list[dict] = []
    for name, sample in corner_samples:
        flat = sample.reshape(-1, 3)
        if flat.size == 0:
            corners_detail.append(
                {
                    "corner": name,
                    "rgb": (0, 0, 0),
                    "best": "none",
                    "distance": float("inf"),
                }
            )
            continue
        med = np.median(flat, axis=0)
        best_name, best_d = "none", float("inf")
        for n, c in GEM_CENTROIDS.items():
            d = _rgb_distance(med, c)
            if d < best_d:
                best_d, best_name = d, n
        corners_detail.append(
            {
                "corner": name,
                "rgb": (int(med[0]), int(med[1]), int(med[2])),
                "best": best_name,
                "distance": round(best_d, 1),
            }
        )

    all_pixels = _sample_corner_pixels(tile_img)
    overall = np.median(all_pixels, axis=0) if all_pixels.size else np.zeros(3)
    distances = {
        n: round(_rgb_distance(overall, c), 1) for n, c in GEM_CENTROIDS.items()
    }

    return {
        "corners": corners_detail,
        "overall_rgb": (int(overall[0]), int(overall[1]), int(overall[2])),
        "distances": distances,
        "status": detect_status(tile_img),
        "guess": detect_gem(tile_img),
    }


LETTER_INSET_FRAC = 0.22


def _letter_region(tile_img: Image.Image) -> Image.Image:
    w, h = tile_img.size
    ix = int(w * LETTER_INSET_FRAC)
    iy = int(h * LETTER_INSET_FRAC)
    return tile_img.crop((ix, iy, w - ix, h - iy))


def recognise_tile(
    tile_img: Image.Image, db: TileDB, distance_threshold: int | None = None
) -> Recognition:
    status = detect_status(tile_img)
    h = phash(_letter_region(tile_img))
    gem = detect_gem(tile_img) if status == "normal" else "none"
    ent = db.lookup(h, max_distance=distance_threshold)
    if ent is None:
        return Recognition(
            letter="", gem=gem, hash=h, distance=-1, confident=False, status=status
        )
    distance = hamming(h, int(ent.hash, 16))
    return Recognition(
        letter=ent.letter,
        gem=gem,
        hash=h,
        distance=distance,
        confident=(distance == 0),
        status=status,
    )


def recognise_rack(tiles: list[Image.Image], db: TileDB) -> list[Recognition]:
    return [recognise_tile(t, db) for t in tiles]


def label_unknowns_interactively(
    tiles: list[Image.Image],
    recognitions: list[Recognition],
    db: TileDB,
    parent=None,
    geometry: str | None = None,
) -> list[Recognition]:
    import tkinter as tk
    from PIL import ImageTk

    owns_root = parent is None
    if owns_root:
        parent = tk.Tk()
        parent.withdraw()

    if geometry is None:
        parent.update_idletasks()
        sw = parent.winfo_screenwidth()
        geometry = f"+{sw - 380}+40"

    updated = list(recognitions)
    try:
        for i, (img, rec) in enumerate(zip(tiles, recognitions)):
            if rec.confident:
                continue

            win = tk.Toplevel(parent)
            win.title(
                f"Label tile {i}  (best guess: {rec.letter or '?'} dist={rec.distance})"
            )
            win.attributes("-topmost", True)
            win.transient(parent)
            win.grab_set()
            win.geometry(geometry)

            disp = img.resize(
                (min(256, img.size[0] * 4), min(256, img.size[1] * 4)), Image.NEAREST
            )
            tk_img = ImageTk.PhotoImage(disp, master=win)
            lbl = tk.Label(win, image=tk_img)
            lbl.image = tk_img
            lbl.pack(padx=10, pady=10)

            entry_var = tk.StringVar(value=rec.letter)
            msg = tk.Label(win, text="Enter the letter shown (A-Z), then press Enter:")
            msg.pack()
            e = tk.Entry(win, textvariable=entry_var, width=6, font=("Helvetica", 24))
            e.pack(pady=10)
            e.focus_set()

            result: dict = {}

            def submit(event=None, w=win, r=result, v=entry_var):
                val = v.get().strip()
                if val and val[0].isalpha():
                    r["letter"] = val.upper() if len(val) == 1 else val
                w.destroy()

            def skip(event=None, w=win):
                w.destroy()

            e.bind("<Return>", submit)
            win.bind("<Escape>", skip)
            win.protocol("WM_DELETE_WINDOW", skip)

            parent.wait_window(win)

            if "letter" in result:
                db.add(rec.hash, result["letter"], rec.gem)
                updated[i] = Recognition(
                    letter=result["letter"],
                    gem=rec.gem,
                    hash=rec.hash,
                    distance=0,
                    confident=True,
                    status=rec.status,
                )
    finally:
        if owns_root:
            parent.destroy()
    return updated


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("tile_dir", help="Directory with tile_NN.png files")
    ap.add_argument("--label", action="store_true", help="Prompt for any unknown tiles")
    ap.add_argument(
        "--inspect",
        action="store_true",
        help="Print HSV stats per tile for tuning gem detection",
    )
    args = ap.parse_args()

    tile_dir = Path(args.tile_dir)
    tiles = []
    for i in range(16):
        p = tile_dir / f"tile_{i:02d}.png"
        if not p.exists():
            print(f"Missing {p}")
            continue
        tiles.append(Image.open(p))

    if args.inspect:
        print(
            f"{'idx':>3}  {'status':>8}  {'guess':>10}    overall RGB per-corner best"
        )
        for i, t in enumerate(tiles):
            info = inspect_tile(t)
            corners_str = "  ".join(
                f"{c['corner']}={c['best']:<8}(d={c['distance']:.0f})"
                for c in info["corners"]
            )
            rgb = info["overall_rgb"]
            print(
                f"  {i:2d}  {info['status']:>8}  {info['guess']:>10}    "
                f"({rgb[0]:3d},{rgb[1]:3d},{rgb[2]:3d})    {corners_str}"
            )
        print()
        print(f"  Centroids loaded from gem_centroids.json:")
        for n, c in GEM_CENTROIDS.items():
            print(f"    {n:<10} RGB=({c[0]:3d}, {c[1]:3d}, {c[2]:3d})")
        print(f"  Distance threshold: {GEM_DISTANCE_THRESHOLD}")

    db = TileDB()
    print(f"DB has {len(db)} entries")
    recs = recognise_rack(tiles, db)
    for i, r in enumerate(recs):
        print(f"  [{i:2d}] letter={r.letter or '?'} gem={r.gem:<8} dist={r.distance}")

    if args.label:
        label_unknowns_interactively(tiles, recs, db)
        print(f"DB now has {len(db)} entries")

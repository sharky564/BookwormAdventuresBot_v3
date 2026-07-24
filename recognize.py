from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image


_SCRIPT_DIR = Path(__file__).resolve().parent
LEGACY_TILE_DB_PATH = _SCRIPT_DIR / "wordgame_tiles.json"
TILE_DB_DIR = _SCRIPT_DIR / "wordgame_tiles"
TILE_DB_PATH = TILE_DB_DIR
TILE_W = 50
TILE_H = 51
PIP_MASK_REGION = None # (0.78, 0.78, 1.00, 1.00)


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
GEM_DISTANCE_THRESHOLD = 70


def _load_centroids() -> dict[str, tuple[int, int, int]]:
    if not _CENTROIDS_PATH.exists():
        return {
            "none": (230, 217, 159),
            "amethyst": (254, 100, 255),
            "emerald": (74, 151, 78),
            "sapphire": (55, 134, 247),
            "garnet": (254, 146, 55),
            "ruby": (255, 73, 84),
            "crystal": (255, 194, 252),
            "diamond": (255, 246, 245),
        }
    raw = json.loads(_CENTROIDS_PATH.read_text())
    return {name: (c["r"], c["g"], c["b"]) for name, c in raw["centroids"].items()}


GEM_CENTROIDS = _load_centroids()


PLAGUE_GREEN_HUE_RANGE = (90, 160)
PLAGUE_GREEN_SAT_MIN = 0.40
PLAGUE_GREEN_VAL_MIN = 0.35
PLAGUE_PIXEL_FRACTION_MIN = 0.05
PLAGUE_SPREAD_FRAC_MIN = 0.15

CHAIN_SAT_MAX = 0.25
CHAIN_VAL_RANGE = (0.10, 0.55)
CHAIN_PIXEL_FRACTION_MIN = 0.05
LOCKED_DIAGONAL_FRAC_MIN = 0.65
LOCKED_DIAGONAL_BAND_FRAC = 0.12

WILDCARD_VARIANCE_MIN = 6000


def _corner_slices(h: int, w: int) -> tuple[slice, slice, slice, slice]:
    cx_lo = max(3, int(w * 0.15))
    cx_hi = max(8, int(w * 0.28))
    cy_lo = max(3, int(h * 0.15))
    cy_hi = max(8, int(h * 0.28))
    return cy_lo, cy_hi, cx_lo, cx_hi


def _corner_medians_arr(arr: np.ndarray) -> np.ndarray:
    h, w, _ = arr.shape
    cy_lo, cy_hi, cx_lo, cx_hi = _corner_slices(h, w)
    p = (cy_hi - cy_lo) * (cx_hi - cx_lo)
    if p == 0:
        return np.zeros((4, 3), dtype=arr.dtype)
    patches = np.empty((4, p, 3), dtype=arr.dtype)
    patches[0] = arr[cy_lo:cy_hi, cx_lo:cx_hi, :].reshape(-1, 3)
    patches[1] = arr[cy_lo:cy_hi, w - cx_hi : w - cx_lo, :].reshape(-1, 3)
    patches[2] = arr[h - cy_hi : h - cy_lo, cx_lo:cx_hi, :].reshape(-1, 3)
    patches[3] = arr[h - cy_hi : h - cy_lo, w - cx_hi : w - cx_lo, :].reshape(-1, 3)
    return np.median(patches, axis=1)


def _is_wildcard_meds(meds: np.ndarray) -> bool:
    if meds.size == 0:
        return False
    variance = float(np.sum(np.var(meds, axis=0)))
    return variance >= WILDCARD_VARIANCE_MIN


def _detect_status_and_gem_arr(
    arr_255: np.ndarray, gray: np.ndarray
) -> tuple[str, str]:
    h, w, _ = arr_255.shape
    arr_norm = arr_255 * (1.0 / 255.0)

    meds = _corner_medians_arr(arr_255)
    is_wild = _is_wildcard_meds(meds)

    if is_wild:
        gem = "none"
    else:
        pixels = _sample_corner_pixels_arr(arr_255)
        if pixels.size == 0:
            gem = "none"
        else:
            median_rgb = np.median(pixels, axis=0)
            _ensure_gem_arr()
            diff = _GEM_CENTROIDS_ARR - median_rgb
            sq = np.einsum("ij,ij->i", diff, diff)
            best_idx = int(np.argmin(sq))
            best_dist = float(np.sqrt(sq[best_idx]))
            gem = (
                _GEM_NAMES[best_idx] if best_dist <= GEM_DISTANCE_THRESHOLD else "none"
            )

    # A plague tile's green wash often reaches the corner samples and
    # masquerades as an emerald match; the gem short-circuit would then skip
    # the plague test entirely. Emerald matches therefore only count after
    # surviving the green-spread test below.
    if is_wild or (gem != "none" and gem != "emerald"):
        return "normal", gem

    fr2 = arr_norm[:, :, 0]
    fg2 = arr_norm[:, :, 1]
    fb2 = arr_norm[:, :, 2]
    cmax2 = arr_norm.max(axis=2)
    delta2 = cmax2 - arr_norm.min(axis=2)

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

    green_mask = (
        (PLAGUE_GREEN_HUE_RANGE[0] <= hue2)
        & (hue2 <= PLAGUE_GREEN_HUE_RANGE[1])
        & (sat2 >= PLAGUE_GREEN_SAT_MIN)
        & (val2 >= PLAGUE_GREEN_VAL_MIN)
    )
    total_green = int(green_mask.sum())
    if total_green / green_mask.size >= PLAGUE_PIXEL_FRACTION_MIN:
        ys, xs = np.where(green_mask)
        if len(xs) >= 3:
            std_x = float(np.std(xs))
            std_y = float(np.std(ys))
            if (
                std_x >= w * PLAGUE_SPREAD_FRAC_MIN
                and std_y >= h * PLAGUE_SPREAD_FRAC_MIN
            ):
                return "plague", "none"

    if gem == "emerald":
        return "normal", gem  # survived the plague test: a real emerald

    chain = (
        (sat2 < CHAIN_SAT_MAX)
        & (val2 > CHAIN_VAL_RANGE[0])
        & (val2 < CHAIN_VAL_RANGE[1])
    )
    chain_count = int(chain.sum())
    if chain_count / chain.size < CHAIN_PIXEL_FRACTION_MIN:
        return "normal", gem

    yy, xx = np.indices((h, w), dtype=np.float32)
    diag1_dist = np.abs(yy * w - xx * h) / np.sqrt(h * h + w * w)
    diag2_dist = np.abs(yy * w - (w - 1 - xx) * h) / np.sqrt(h * h + w * w)
    band = max(h, w) * LOCKED_DIAGONAL_BAND_FRAC
    near_diag = (diag1_dist < band) | (diag2_dist < band)
    diag_chain_frac = int((chain & near_diag).sum()) / max(1, chain_count)

    if diag_chain_frac >= LOCKED_DIAGONAL_FRAC_MIN:
        return "locked", gem
    return "smashed", gem


def detect_status(tile_img: Image.Image) -> str:
    arr_255 = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    gray = np.asarray(tile_img.convert("L"), dtype=np.uint8)
    status, _ = _detect_status_and_gem_arr(arr_255, gray)
    return status


def _sample_corner_pixels_arr(arr: np.ndarray) -> np.ndarray:
    h, w, _ = arr.shape
    cy_lo, cy_hi, cx_lo, cx_hi = _corner_slices(h, w)
    patches = [
        arr[cy_lo:cy_hi, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[cy_lo:cy_hi, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, cx_lo:cx_hi, :].reshape(-1, 3),
        arr[h - cy_hi : h - cy_lo, w - cx_hi : w - cx_lo, :].reshape(-1, 3),
    ]
    return np.concatenate(patches, axis=0)


def _sample_corner_pixels(tile_img: Image.Image) -> np.ndarray:
    arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    return _sample_corner_pixels_arr(arr)


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


_GEM_NAMES: list[str] = []
_GEM_CENTROIDS_ARR: np.ndarray | None = None


def _ensure_gem_arr() -> None:
    global _GEM_CENTROIDS_ARR
    if _GEM_CENTROIDS_ARR is None:
        names = list(GEM_CENTROIDS.keys())
        centroids = np.asarray([GEM_CENTROIDS[n] for n in names], dtype=np.float32)
        _GEM_NAMES.extend(names)
        _GEM_CENTROIDS_ARR = centroids


def _detect_gem_arr(arr: np.ndarray) -> str:
    meds = _corner_medians_arr(arr)
    if _is_wildcard_meds(meds):
        return "none"

    pixels = _sample_corner_pixels_arr(arr)
    if pixels.size == 0:
        return "none"
    median_rgb = np.median(pixels, axis=0)

    _ensure_gem_arr()
    diff = _GEM_CENTROIDS_ARR - median_rgb
    sq = np.einsum("ij,ij->i", diff, diff)
    best_idx = int(np.argmin(sq))
    best_dist = float(np.sqrt(sq[best_idx]))
    if best_dist > GEM_DISTANCE_THRESHOLD:
        return "none"
    return _GEM_NAMES[best_idx]


def detect_gem(tile_img: Image.Image) -> str:
    arr_255 = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    return _detect_gem_arr(arr_255)


@dataclass
class TileEntry:
    tile_id: str
    letter: str
    gem: str
    status: str = "normal"
    norm: np.ndarray | None = None

    @property
    def state(self) -> str:
        if self.status != "normal":
            return self.status
        if self.gem != "none":
            return self.gem
        return "normal"

    @property
    def hash(self) -> str:
        return self.tile_id


def _normalize_tile_arr(arr: np.ndarray) -> np.ndarray:
    h, w = arr.shape[:2]
    if (w, h) != (TILE_W, TILE_H):
        if arr.ndim == 3:
            img = Image.fromarray(arr.astype(np.uint8), mode="RGB")
        else:
            img = Image.fromarray(arr.astype(np.uint8), mode="L")
        img = img.resize((TILE_W, TILE_H), Image.LANCZOS)
        arr = np.asarray(img, dtype=np.float32)
    if arr.ndim == 3:
        gray = arr[..., :3].mean(axis=-1).astype(np.float32)
    else:
        gray = arr.astype(np.float32)

    if PIP_MASK_REGION is not None:
        x0_f, y0_f, x1_f, y1_f = PIP_MASK_REGION
        x0 = int(TILE_W * x0_f)
        x1 = int(TILE_W * x1_f)
        y0 = int(TILE_H * y0_f)
        y1 = int(TILE_H * y1_f)
        keep = np.ones(gray.shape, dtype=bool)
        keep[y0:y1, x0:x1] = False
        if keep.any():
            fill = float(np.median(gray[keep]))
            gray = gray.copy()
            gray[y0:y1, x0:x1] = fill

    out = gray
    out = out - out.mean()
    s = float(out.std())
    if s > 1e-6:
        out = out / s
    return out


def normalize_tile(tile_img: Image.Image) -> np.ndarray:
    arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    return _normalize_tile_arr(arr)


class TileDB:

    DB_SCHEMA = 5
    K_NEIGHBOURS = 3
    MAX_DISTANCE = 22.0
    CONFIDENCE_MARGIN = 1.5
    TRUST_BANK_STATE_DISTANCE = 6.0

    def __init__(self, path: Path = TILE_DB_DIR):
        self.path = Path(path)
        self.entries: list[TileEntry] = []
        self._by_id: dict[str, TileEntry] = {}
        self._stack: np.ndarray | None = None
        self._stack_ids: list[str] = []

        self._migrate_legacy_if_present()
        self._load_manifest()

    def _migrate_legacy_if_present(self) -> None:
        legacy = LEGACY_TILE_DB_PATH
        if not legacy.exists():
            return
        try:
            raw = json.loads(legacy.read_text())
            schema = int(raw.get("schema", 1)) if isinstance(raw, dict) else 1
        except Exception:
            schema = 0
        backup = legacy.with_suffix(f".v{schema}.bak.json")
        i = 1
        while backup.exists():
            backup = legacy.with_suffix(f".v{schema}.bak.{i}.json")
            i += 1
        legacy.rename(backup)
        print(
            f"Tile DB v{schema} found at {legacy.name}; archived to "
            f"{backup.name}. The bank will now store raw tile images "
            f"(schema v{self.DB_SCHEMA}); please re-label tiles as you encounter them."
        )

    def _manifest_path(self) -> Path:
        return self.path / "manifest.json"

    def _png_path(self, tile_id: str) -> Path:
        return self.path / f"{tile_id}.png"

    def _load_manifest(self) -> None:
        mf = self._manifest_path()
        if not mf.exists():
            return
        try:
            raw = json.loads(mf.read_text())
        except Exception as e:
            print(f"Tile DB manifest load failed ({e}); starting fresh.")
            return
        schema = int(raw.get("schema", 0))
        if schema != self.DB_SCHEMA:
            backup = mf.with_suffix(f".v{schema}.bak.json")
            i = 1
            while backup.exists():
                backup = mf.with_suffix(f".v{schema}.bak.{i}.json")
                i += 1
            mf.rename(backup)
            print(
                f"Tile DB manifest at schema v{schema}; archived to "
                f"{backup.name}. Starting fresh at v{self.DB_SCHEMA}."
            )
            return
        for e in raw.get("entries", []):
            ent = TileEntry(
                tile_id=str(e["id"]),
                letter=e["letter"],
                gem=e.get("gem", "none"),
                status=e.get("status", "normal"),
            )
            self.entries.append(ent)
            self._by_id[ent.tile_id] = ent

    def save_manifest(self) -> None:
        self.path.mkdir(parents=True, exist_ok=True)
        payload = {
            "schema": self.DB_SCHEMA,
            "entries": [
                {
                    "id": e.tile_id,
                    "letter": e.letter,
                    "gem": e.gem,
                    "status": e.status,
                }
                for e in self.entries
            ],
        }
        self._manifest_path().write_text(json.dumps(payload, indent=2))

    def _ensure_loaded(self) -> None:
        if (
            self._stack is not None
            and len(self._stack_ids) == len(self.entries)
            and all(
                self._stack_ids[i] == self.entries[i].tile_id
                for i in range(len(self.entries))
            )
        ):
            return
        norms: list[np.ndarray] = []
        ids: list[str] = []
        for ent in self.entries:
            if ent.norm is None:
                png = self._png_path(ent.tile_id)
                if not png.exists():
                    continue
                arr = np.asarray(Image.open(png).convert("RGB"), dtype=np.float32)
                ent.norm = _normalize_tile_arr(arr)
            norms.append(ent.norm)
            ids.append(ent.tile_id)
        if norms:
            self._stack = np.stack(norms, axis=0).reshape(len(norms), -1)
        else:
            self._stack = None
        self._stack_ids = ids


    def _new_tile_id(self, letter: str) -> str:
        existing_ids = set(self._by_id.keys())
        i = 1
        while True:
            candidate = f"{letter.upper()}_{i:04d}"
            if candidate not in existing_ids:
                return candidate
            i += 1

    def add(
        self,
        tile_img: Image.Image,
        letter: str,
        gem: str = "none",
        status: str = "normal",
    ) -> TileEntry:
        self.path.mkdir(parents=True, exist_ok=True)
        tile_id = self._new_tile_id(letter)
        tile_img.convert("RGB").save(self._png_path(tile_id))
        arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
        ent = TileEntry(
            tile_id=tile_id,
            letter=letter.upper(),
            gem=gem,
            status=status,
            norm=_normalize_tile_arr(arr),
        )
        self.entries.append(ent)
        self._by_id[tile_id] = ent
        self._stack = None
        self.save_manifest()
        return ent

    def lookup(self, tile_img: Image.Image) -> tuple[TileEntry | None, float]:
        self._ensure_loaded()
        if self._stack is None or self._stack.shape[0] == 0:
            return None, float("inf")

        arr = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
        qnorm = _normalize_tile_arr(arr).reshape(-1)

        diffs = self._stack - qnorm
        dists = np.sqrt(np.einsum("ij,ij->i", diffs, diffs))

        order = np.argsort(dists)
        K = min(self.K_NEIGHBOURS, len(order))
        top_k = order[:K]
        closest_d = float(dists[top_k[0]])

        if closest_d > self.MAX_DISTANCE:
            return None, closest_d

        votes: dict[str, float] = {}
        for idx in top_k:
            ent = self.entries[idx]
            d = float(dists[idx])
            votes[ent.letter] = votes.get(ent.letter, 0.0) + 1.0 / (d + 0.01)

        ranked = sorted(votes.items(), key=lambda kv: -kv[1])
        top_letter, top_score = ranked[0]
        if len(ranked) > 1:
            runner_score = ranked[1][1]
            if top_score < runner_score * self.CONFIDENCE_MARGIN:
                return None, closest_d

        for idx in top_k:
            if self.entries[idx].letter == top_letter:
                return self.entries[idx], float(dists[idx])
        return self.entries[top_k[0]], closest_d

    def __len__(self) -> int:
        return len(self.entries)


GEM_STATES = ("amethyst", "emerald", "sapphire", "garnet", "ruby", "crystal", "diamond")
AFFLICTION_STATES = ("smashed", "locked", "plague")
BROKEN_STATES = ("smashed", "plague")  # still playable, but score 0 damage
ALL_TILE_STATES = ("normal",) + GEM_STATES + AFFLICTION_STATES
PLAYABLE_STATES = ("normal",) + GEM_STATES


def state_to_gem_status(state: str) -> tuple[str, str]:
    if state == "normal":
        return "none", "normal"
    if state in GEM_STATES:
        return state, "normal"
    if state in AFFLICTION_STATES:
        return "none", state
    raise ValueError(
        f"Unknown tile state: {state!r}; expected one of {ALL_TILE_STATES}"
    )


@dataclass
class Recognition:

    letter: str
    gem: str
    distance: float
    confident: bool
    status: str = "normal"

    @property
    def state(self) -> str:
        if self.status != "normal":
            return self.status
        if self.gem != "none":
            return self.gem
        return "normal"

    @property
    def playable(self) -> bool:
        return self.status == "normal"


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


def recognise_tile(
    tile_img: Image.Image, db: TileDB, distance_threshold: float | None = None
) -> Recognition:

    arr_255 = np.asarray(tile_img.convert("RGB"), dtype=np.float32)
    gray = np.asarray(tile_img.convert("L"), dtype=np.uint8)

    live_status, live_gem_raw = _detect_status_and_gem_arr(arr_255, gray)
    live_gem = live_gem_raw if live_status == "normal" else "none"

    if distance_threshold is not None:
        old_thresh = db.MAX_DISTANCE
        db.MAX_DISTANCE = distance_threshold
        try:
            ent, dist = db.lookup(tile_img)
        finally:
            db.MAX_DISTANCE = old_thresh
    else:
        ent, dist = db.lookup(tile_img)

    if ent is not None and dist <= db.TRUST_BANK_STATE_DISTANCE:
        gem = ent.gem
        status = ent.status
    else:
        gem = live_gem
        status = live_status

    if ent is None:
        return Recognition(
            letter="", gem=gem, distance=dist, confident=False, status=status
        )
    return Recognition(
        letter=ent.letter,
        gem=gem,
        distance=dist,
        confident=True,
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
                f"Label tile {i}  (best guess: {rec.letter or '?'} dist={rec.distance:.1f})"
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
                db.add(
                    img,
                    result["letter"],
                    gem=rec.gem,
                    status=rec.status,
                )
                updated[i] = Recognition(
                    letter=result["letter"],
                    gem=rec.gem,
                    distance=0.0,
                    confident=True,
                    status=rec.status,
                )
    finally:
        if owns_root:
            parent.destroy()
    return updated


def correct_recognition(
    tile_img: Image.Image,
    db: TileDB,
    letter: str,
    state: str = "normal",
) -> Recognition:
    gem, status = state_to_gem_status(state)
    db.add(tile_img, letter, gem=gem, status=status)
    return Recognition(
        letter=letter.upper(),
        gem=gem,
        distance=0.0,
        confident=True,
        status=status,
    )


def review_rack_interactively(
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
        geometry = f"+{sw - 420}+40"

    updated = list(recognitions)
    try:
        for i, (img, rec) in enumerate(zip(tiles, recognitions)):
            win = tk.Toplevel(parent)
            win.title(
                f"Review tile {i + 1}/{len(tiles)}  (detected: "
                f"{rec.letter or '?'} / {rec.state}, dist={rec.distance:.1f})"
            )
            win.attributes("-topmost", True)
            win.transient(parent)
            win.grab_set()
            win.geometry(geometry)

            disp = img.resize(
                (min(256, img.size[0] * 4), min(256, img.size[1] * 4)),
                Image.NEAREST,
            )
            tk_img = ImageTk.PhotoImage(disp, master=win)
            lbl = tk.Label(win, image=tk_img)
            lbl.image = tk_img
            lbl.pack(padx=10, pady=10)

            letter_frame = tk.Frame(win)
            letter_frame.pack(pady=2)
            tk.Label(letter_frame, text="Letter:").pack(side="left", padx=(0, 4))
            letter_var = tk.StringVar(value=rec.letter)
            letter_entry = tk.Entry(
                letter_frame, textvariable=letter_var, width=4, font=("Helvetica", 18)
            )
            letter_entry.pack(side="left")

            state_frame = tk.Frame(win)
            state_frame.pack(pady=2)
            tk.Label(state_frame, text="State:").pack(side="left", padx=(0, 4))
            state_var = tk.StringVar(value=rec.state)
            state_menu = tk.OptionMenu(state_frame, state_var, *ALL_TILE_STATES)
            state_menu.config(width=12)
            state_menu.pack(side="left")

            tk.Label(
                win,
                text="Enter = save & next   Esc = skip & next",
                fg="#666",
            ).pack(pady=(8, 4))

            result: dict = {}

            def submit(event=None, w=win, r=result, lv=letter_var, sv=state_var):
                letter = lv.get().strip()
                state = sv.get().strip()
                if not letter or not letter[0].isalpha():
                    w.destroy()
                    return
                if state not in ALL_TILE_STATES:
                    w.destroy()
                    return
                r["letter"] = letter.upper() if len(letter) == 1 else letter
                r["state"] = state
                w.destroy()

            def skip(event=None, w=win):
                w.destroy()

            letter_entry.bind("<Return>", submit)
            win.bind("<Return>", submit)
            win.bind("<Escape>", skip)
            win.protocol("WM_DELETE_WINDOW", skip)
            letter_entry.focus_set()
            letter_entry.select_range(0, "end")

            parent.wait_window(win)

            if "letter" in result and "state" in result:
                new_letter = result["letter"]
                new_state = result["state"]
                if new_letter != rec.letter or new_state != rec.state:
                    updated[i] = correct_recognition(img, db, new_letter, new_state)
                else:
                    pass
    finally:
        if owns_root:
            parent.destroy()
    return updated


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("tile_dir", help="Directory with tile_NN.png files")
    ap.add_argument(
        "--label",
        action="store_true",
        help="Prompt for any unknown tiles (only the ones the bank couldn't confidently match)",
    )
    ap.add_argument(
        "--review",
        action="store_true",
        help="Review and possibly override the recognition for every tile in the rack",
    )
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
        print("  Centroids loaded from gem_centroids.json:")
        for n, c in GEM_CENTROIDS.items():
            print(f"    {n:<10} RGB=({c[0]:3d}, {c[1]:3d}, {c[2]:3d})")
        print(f"  Distance threshold: {GEM_DISTANCE_THRESHOLD}")

    db = TileDB()
    print(f"DB has {len(db)} entries")
    recs = recognise_rack(tiles, db)
    for i, r in enumerate(recs):
        print(
            f"  [{i:2d}] letter={r.letter or '?'} state={r.state:<10} "
            f"dist={r.distance:.1f}  confident={r.confident}"
        )

    if args.label:
        recs = label_unknowns_interactively(tiles, recs, db)
        print(f"DB now has {len(db)} entries")
    if args.review:
        recs = review_rack_interactively(tiles, recs, db)
        print(f"DB now has {len(db)} entries")
        for i, r in enumerate(recs):
            print(f"  [{i:2d}] letter={r.letter or '?'} state={r.state:<10}")

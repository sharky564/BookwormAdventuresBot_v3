from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PIL import Image
import numpy as np
from recognize import detect_gem, detect_status


FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures"


@dataclass
class TileExpect:
    row: int
    col: int
    letter: str
    gem: str
    status: str


@dataclass
class Fixture:
    name: str
    description: str
    image_path: Path
    rack_offset_x: int
    rack_offset_y: int
    tile_size_x: int
    tile_size_y: int
    tiles: list[TileExpect]

    @classmethod
    def load(cls, json_path: Path) -> "Fixture":
        d = json.loads(json_path.read_text())
        return cls(
            name=json_path.stem,
            description=d.get("description", ""),
            image_path=json_path.with_name(d["image"]),
            rack_offset_x=int(d.get("rack_offset_x", 0)),
            rack_offset_y=int(d.get("rack_offset_y", 0)),
            tile_size_x=int(d["tile_size_x"]),
            tile_size_y=int(d["tile_size_y"]),
            tiles=[TileExpect(**t) for t in d["tiles"]],
        )

    def crop_tile(self, img: Image.Image, row: int, col: int) -> Image.Image:
        l = self.rack_offset_x + col * self.tile_size_x
        t = self.rack_offset_y + row * self.tile_size_y
        return img.crop((l, t, l + self.tile_size_x, t + self.tile_size_y))


@dataclass
class TileResult:
    expected: TileExpect
    got_gem: str
    got_status: str
    gem_ok: bool
    status_ok: bool


def run_fixture(fix: Fixture) -> list[TileResult]:
    img = Image.open(fix.image_path).convert("RGB")
    results: list[TileResult] = []
    for exp in fix.tiles:
        tile = fix.crop_tile(img, exp.row, exp.col)
        got_gem = detect_gem(tile)
        got_status = detect_status(tile)
        if exp.status != "normal":
            gem_ok = True
        else:
            gem_ok = got_gem == exp.gem
        results.append(
            TileResult(
                expected=exp,
                got_gem=got_gem,
                got_status=got_status,
                gem_ok=gem_ok,
                status_ok=(got_status == exp.status),
            )
        )
    return results


def report_fixture(
    fix: Fixture, results: list[TileResult], *, verbose: bool, gems_only: bool
) -> tuple[int, int]:
    gem_ok = sum(1 for r in results if r.gem_ok)
    status_ok = sum(1 for r in results if r.status_ok)
    total = len(results)
    if gems_only:
        all_ok = gem_ok == total
    else:
        all_ok = gem_ok == total and status_ok == total

    mark = "PASS" if all_ok else "FAIL"
    print(
        f"[{mark}] {fix.name}  gem {gem_ok}/{total}  status {status_ok}/{total}"
        + (f"  — {fix.description}" if fix.description else "")
    )

    if verbose or not all_ok:
        for r in results:
            if r.gem_ok and r.status_ok:
                continue
            e = r.expected
            print(
                f"      [{e.row},{e.col}] {e.letter} "
                f"exp gem={e.gem:<10} got={r.got_gem:<10}  "
                f"exp status={e.status:<8} got={r.got_status}"
            )

    n_pass = gem_ok if gems_only else (gem_ok + status_ok)
    n_total = total if gems_only else 2 * total
    return n_pass, n_total


def learn_centroids(fixtures: list[Fixture]) -> dict:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from recognize import _sample_corner_pixels  # noqa: E402

    samples: dict[str, list[np.ndarray]] = {}
    for fix in fixtures:
        img = Image.open(fix.image_path).convert("RGB")
        for t in fix.tiles:
            if t.status != "normal":
                continue  # smashed/locked/plague tiles aren't reliable gem refs
            tile = fix.crop_tile(img, t.row, t.col)
            samples.setdefault(t.gem, []).append(_sample_corner_pixels(tile))

    centroids: dict[str, dict] = {}
    for gem, lst in samples.items():
        all_pixels = np.concatenate(lst, axis=0)
        med = np.median(all_pixels, axis=0)
        centroids[gem] = {
            "r": int(med[0]),
            "g": int(med[1]),
            "b": int(med[2]),
            "n": len(lst),
        }
    return centroids


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "filters",
        nargs="*",
        help="Substring filters on fixture names. If omitted, runs all.",
    )
    ap.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Show every tile result, not just failures.",
    )
    ap.add_argument(
        "--gems-only",
        action="store_true",
        help="Only check gems; skip status checking.",
    )
    ap.add_argument(
        "--learn-centroids",
        action="store_true",
        help="Refit gem_centroids.json from labelled fixtures and exit.",
    )
    ap.add_argument(
        "--inspect",
        nargs=3,
        metavar=("FIXTURE", "ROW", "COL"),
        help="Print per-tile diagnostics: per-corner RGB and "
        "distance to each centroid. Useful when figuring "
        "out why a tile is being misclassified.",
    )
    args = ap.parse_args()

    if not FIXTURES_DIR.exists():
        print(f"No fixtures directory at {FIXTURES_DIR}", file=sys.stderr)
        return 1

    json_files = sorted(FIXTURES_DIR.glob("*.json"))
    if args.filters:
        json_files = [j for j in json_files if any(f in j.stem for f in args.filters)]

    if not json_files:
        print("No matching fixtures.", file=sys.stderr)
        return 1

    if args.inspect:
        from recognize import inspect_tile

        fixture_name, row_s, col_s = args.inspect
        row, col = int(row_s), int(col_s)
        matches = [j for j in json_files if fixture_name in j.stem]
        if not matches:
            print(f"No fixture matching '{fixture_name}'.", file=sys.stderr)
            return 1
        fix = Fixture.load(matches[0])
        img = Image.open(fix.image_path).convert("RGB")
        tile = fix.crop_tile(img, row, col)
        info = inspect_tile(tile)

        exp = next((t for t in fix.tiles if t.row == row and t.col == col), None)
        if exp is not None:
            print(f"Tile [{row}, {col}] in {fix.name}")
            print(
                f"  Expected: letter={exp.letter}  gem={exp.gem}  status={exp.status}"
            )
        else:
            print(f"Tile [{row}, {col}] in {fix.name} (not annotated)")
        print(f"  Detected: status={info['status']}  gem={info['guess']}")
        print(f"  Overall median RGB: {info['overall_rgb']}")
        print("  Per-corner detail:")
        for c in info["corners"]:
            r, g, b = c["rgb"]
            print(
                f"    {c['corner']}  RGB=({r:3d},{g:3d},{b:3d})  "
                f"closest={c['best']:<10}  d={c['distance']:.0f}"
            )
        print("  Overall distance to each centroid (sorted):")
        for name, d in sorted(info["distances"].items(), key=lambda kv: kv[1]):
            r, g, b = info["overall_rgb"]
            cr, cg, cb = (None, None, None)
            try:
                from recognize import GEM_CENTROIDS

                cr, cg, cb = GEM_CENTROIDS[name]
            except Exception:
                pass
            print(f"    {name:<10}  d={d:6.1f}  (centroid RGB=({cr},{cg},{cb}))")
        return 0

    if args.learn_centroids:
        fixtures = [Fixture.load(jp) for jp in json_files]
        new_centroids = learn_centroids(fixtures)
        out_path = Path(__file__).resolve().parent.parent / "gem_centroids.json"
        existing = json.loads(out_path.read_text()) if out_path.exists() else {}
        merged: dict[str, dict] = dict(existing.get("centroids", {}))
        for gem, c in new_centroids.items():
            merged[gem] = c
        payload = {
            "_comment": existing.get(
                "_comment",
                "RGB centroids per gem. Refit with `python tests/test_recognition.py --learn-centroids`.",
            ),
            "centroids": merged,
        }
        out_path.write_text(json.dumps(payload, indent=2))
        print(f"Updated {out_path}:")
        for gem, c in merged.items():
            marker = "*" if gem in new_centroids else " "
            n = c.get("n", "?")
            print(
                f"  {marker} {gem:<10}  RGB=({c['r']:3d}, {c['g']:3d}, {c['b']:3d})  n={n}"
            )
        print("  (* = refit from fixtures in this run)")
        return 0

    total_pass = 0
    total_total = 0
    fixtures_passed = 0
    for jp in json_files:
        fix = Fixture.load(jp)
        results = run_fixture(fix)
        p, t = report_fixture(
            fix, results, verbose=args.verbose, gems_only=args.gems_only
        )
        total_pass += p
        total_total += t
        if all((r.gem_ok and (args.gems_only or r.status_ok)) for r in results):
            fixtures_passed += 1

    print()
    print(f"Fixtures:  {fixtures_passed}/{len(json_files)} passed")
    print(
        f"Tile checks: {total_pass}/{total_total} passed "
        f"({100 * total_pass / max(1, total_total):.1f}%)"
    )
    return 0 if fixtures_passed == len(json_files) else 1


if __name__ == "__main__":
    sys.exit(main())

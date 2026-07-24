from __future__ import annotations

import json
import os
import subprocess
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Tile:
    letter: str
    gem: str = "none"
    broken: bool = False  # smashed/plagued: spells normally, scores 0


@dataclass
class ScoredWord:
    word: str
    now: int
    future: float
    total: float
    tiles: list[Tile]
    n_sims: int
    is_kill: bool = False
    used_powered: bool = False


class EngineError(RuntimeError):
    pass


class Engine:
    """Long-lived subprocess. Use as a context manager or call close()."""

    # Features this Python side depends on. The solver advertises its feature
    # set in the ready line; missing entries mean a stale binary whose parser
    # would silently ignore the corresponding request fields (e.g. a solver
    # without "broken" would score smashed tiles at full damage).
    REQUIRED_FEATURES = frozenset({"broken", "engine", "prefilter_k"})

    def __init__(self, exe_path: str | os.PathLike, dict_path: str | os.PathLike):
        exe = Path(exe_path)
        if not exe.exists():
            raise FileNotFoundError(f"wordgame executable not found: {exe}")
        if not Path(dict_path).exists():
            raise FileNotFoundError(f"dictionary not found: {dict_path}")
        self._proc_cwd = str(exe.parent.resolve())
        self._proc = subprocess.Popen(
            [str(exe), str(dict_path), "serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            cwd=self._proc_cwd,
        )
        self._wait_for_ready()
        self._io_lock = threading.Lock()
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="engine")

    def _wait_for_ready(self, timeout: float = 30.0) -> None:
        import time

        deadline = time.monotonic() + timeout
        startup_log = []
        while time.monotonic() < deadline:
            line = self._proc.stderr.readline()
            if not line:
                try:
                    self._proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    pass
                rc = self._proc.returncode
                trailing_err = ""
                trailing_out = ""
                try:
                    trailing_err = self._proc.stderr.read() or ""
                except Exception:
                    pass
                try:
                    trailing_out = self._proc.stdout.read() or ""
                except Exception:
                    pass
                seen = "".join(startup_log).rstrip()
                raise EngineError(
                    "engine died before ready:\n"
                    f"  exit code: {rc}\n"
                    f"  argv:      {self._proc.args}\n"
                    f"  cwd:       {self._proc_cwd}\n"
                    f"  stderr seen so far:\n{seen or '    (none)'}\n"
                    f"  stderr trailing: {trailing_err!r}\n"
                    f"  stdout trailing: {trailing_out!r}"
                )
            startup_log.append(line)
            if "ready" in line:
                self.features = self._parse_features(line)
                missing = self.REQUIRED_FEATURES - self.features
                if missing:
                    raise EngineError(
                        f"solver binary is missing protocol features {sorted(missing)} "
                        f"(advertised: {sorted(self.features) or 'none'}). "
                        "Rebuild it: cd solver && make"
                    )
                self._start_stderr_drain()
                return
        seen = "".join(startup_log).rstrip()
        raise EngineError(
            f"timed out after {timeout:.1f}s waiting for engine ready.\n"
            f"stderr seen so far:\n{seen or '    (none)'}"
        )

    @staticmethod
    def _parse_features(ready_line: str) -> frozenset[str]:
        for tok in ready_line.split():
            if tok.startswith("features="):
                return frozenset(f for f in tok[len("features="):].split(",") if f)
        return frozenset()

    def _start_stderr_drain(self) -> None:
        # The solver only writes to stderr at startup today, but if it ever
        # logs steadily, an undrained pipe fills and deadlocks the process.
        def drain() -> None:
            try:
                for _ in self._proc.stderr:
                    pass
            except Exception:
                pass

        threading.Thread(target=drain, daemon=True, name="engine-stderr").start()

    def _send(self, msg: dict) -> dict:
        with self._io_lock:
            if self._proc.poll() is not None:
                raise EngineError(f"engine exited with code {self._proc.returncode}")
            line = json.dumps(msg) + "\n"
            self._proc.stdin.write(line)
            self._proc.stdin.flush()
            resp_line = self._proc.stdout.readline()
            if not resp_line:
                # stderr is owned by the drain thread; don't read it here.
                raise EngineError(
                    f"engine closed stdout (exit code {self._proc.poll()})"
                )
            resp = json.loads(resp_line)
            if "error" in resp:
                raise EngineError(
                    f"engine error ({resp.get('op', '?')}): {resp['error']}"
                )
            return resp

    def ping(self) -> bool:
        return self._send({"op": "ping"}).get("op") == "pong"

    def config(
        self,
        *,
        letter_points: list[float] | None = None,
        gems_enabled: bool | None = None,
        rainbow: bool | None = None,
        prescramble: bool | None = None,
        power: float | None = None,
        powered: bool | None = None,
        base_damage_bonus: int | None = None,
        enemy_armour: int | None = None,
        treasure_equipped: bool | None = None,
        weakness_cat: int | None = None,
        weakness_boost: float | None = None,
    ) -> None:
        msg = {"op": "config"}
        if letter_points is not None:
            if len(letter_points) != 26:
                raise ValueError("letter_points must have 26 entries")
            msg["letter_points"] = list(letter_points)
        if gems_enabled is not None:
            msg["gems_enabled"] = bool(gems_enabled)
        if rainbow is not None:
            msg["rainbow"] = bool(rainbow)
        if prescramble is not None:
            msg["prescramble"] = bool(prescramble)
        if power is not None:
            msg["power"] = float(power)
        if powered is not None:
            msg["powered"] = bool(powered)
        if base_damage_bonus is not None:
            msg["base_damage_bonus"] = int(base_damage_bonus)
        if enemy_armour is not None:
            msg["enemy_armour"] = int(enemy_armour)
        if treasure_equipped is not None:
            msg["treasure_equipped"] = bool(treasure_equipped)
        if weakness_cat is not None:
            msg["weakness_cat"] = int(weakness_cat)
        if weakness_boost is not None:
            msg["weakness_boost"] = float(weakness_boost)
        self._send(msg)

    @staticmethod
    def _parse_tiles(raw: list) -> list[Tile]:
        return [
            Tile(
                letter=t["letter"],
                gem=t.get("gem", "none"),
                broken=bool(t.get("broken", False)),
            )
            for t in raw
        ]

    def best(
        self, rack: str, gems: str | None = None, broken: str | None = None
    ) -> ScoredWord:
        msg = {"op": "best", "rack": rack}
        if gems:
            msg["gems"] = gems
        if broken:
            msg["broken"] = broken
        r = self._send(msg)
        return ScoredWord(
            word=r["word"],
            now=int(r["damage"]),
            future=0.0,
            total=float(r["damage"]),
            tiles=self._parse_tiles(r.get("tiles", [])),
            n_sims=1,
        )

    def top(
        self,
        rack: str,
        gems: str | None = None,
        *,
        n: int = 20,
        horizon: int = 2,
        min_sims: int = 50,
        max_sims: int = 300,
        se_target: float = 0.5,
        threshold: int = 0,
        next_enemy_hp: int = 0,
        terminal: bool = False,
        alpha: float | None = None,
        beta: float | None = None,
        overkill_delay_penalty: float | None = None,
        kill_gamma: float | None = None,
        kill_margin: float | None = None,
        max_kill_candidates: int | None = None,
        charges: int = 0,
        broken: str | None = None,
    ) -> list[ScoredWord]:
        msg = {
            "op": "top",
            "rack": rack,
            "n": n,
            "horizon": horizon,
            "min_sims": min_sims,
            "max_sims": max_sims,
            "se_target": se_target,
        }
        if gems:
            msg["gems"] = gems
        if broken:
            msg["broken"] = broken
        if threshold > 0:
            msg["threshold"] = int(threshold)
        if next_enemy_hp > 0:
            msg["next_enemy_hp"] = int(next_enemy_hp)
        if terminal:
            msg["terminal"] = True
        if alpha is not None:
            msg["alpha"] = float(alpha)
        if beta is not None:
            msg["beta"] = float(beta)
        if overkill_delay_penalty is not None:
            msg["overkill_delay_penalty"] = float(overkill_delay_penalty)
        if kill_gamma is not None:
            msg["kill_gamma"] = float(kill_gamma)
        if kill_margin is not None:
            msg["kill_margin"] = float(kill_margin)
        if max_kill_candidates is not None:
            msg["max_kill_candidates"] = int(max_kill_candidates)
        if charges > 0:
            msg["charges"] = int(charges)
        r = self._send(msg)
        return [
            ScoredWord(
                word=w["word"],
                now=int(w["now"]),
                future=float(w["future"]),
                total=float(w["total"]),
                tiles=self._parse_tiles(w.get("tiles", [])),
                n_sims=int(w["n_sims"]),
                is_kill=bool(w.get("is_kill", False)),
                used_powered=bool(w.get("used_powered", False)),
            )
            for w in r["words"]
        ]

    def top_async(self, *args, **kwargs) -> "Future[list[ScoredWord]]":
        return self._executor.submit(self.top, *args, **kwargs)

    def trace(
        self, word: str, gems: str | None = None, *, powered: bool | None = None
    ) -> dict:
        msg = {"op": "trace", "word": word}
        if gems:
            msg["gems"] = gems
        if powered is not None:
            msg["powered"] = bool(powered)
        return self._send(msg)

    def replay(
        self,
        rack: str,
        thresholds: list[int],
        gems: str | None = None,
        *,
        per_threshold: int = 3,
        max_candidates: int = 500,
        broken: str | None = None,
    ) -> list[dict]:
        msg = {
            "op": "replay",
            "rack": rack,
            "thresholds": [int(t) for t in thresholds],
            "per_threshold": int(per_threshold),
            "max_candidates": int(max_candidates),
        }
        if gems:
            msg["gems"] = gems
        if broken:
            msg["broken"] = broken
        r = self._send(msg)
        results: list[dict] = []
        for tier in r.get("results", []):
            words = [
                ScoredWord(
                    word=w["word"],
                    now=int(w["now"]),
                    future=0.0,
                    total=-float(w.get("overkill", 0)),
                    tiles=self._parse_tiles(w.get("tiles", [])),
                    n_sims=0,
                    is_kill=bool(w.get("is_kill", True)),
                    used_powered=bool(w.get("used_powered", False)),
                )
                for w in tier.get("words", [])
            ]
            results.append({"threshold": int(tier["threshold"]), "words": words})
        return results

    def step(self, rack: str, word: str, gems: str | None = None,
             *, seed: int = 0) -> tuple[str, str]:
        msg = {"op": "step", "rack": rack, "word": word, "seed": int(seed)}
        if gems:
            msg["gems"] = gems
        r = self._send(msg)
        return r.get("rack", ""), r.get("gems", "")

    def close(self) -> None:
        self._executor.shutdown(wait=True)
        if self._proc.poll() is None:
            try:
                self._send({"op": "quit"})
            except Exception:
                pass
            try:
                self._proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self._proc.kill()

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("dict")
    ap.add_argument("--rack", default="RSNLCAOIAMMERAAS")
    args = ap.parse_args()
    with Engine(args.exe, args.dict) as eng:
        assert eng.ping()
        print("ping OK")
        best = eng.best(args.rack)
        print(f"best: {best.word} ({best.now})")
        print("top-5:")
        for w in eng.top(args.rack, n=5, horizon=2, max_sims=80):
            print(
                f"  {w.word:<14} now={w.now:>3} future={w.future:>6.1f} total={w.total:>6.1f} sims={w.n_sims}"
            )
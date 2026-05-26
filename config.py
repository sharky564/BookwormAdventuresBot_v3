from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any
import tomllib


_SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = _SCRIPT_DIR / "wordgame.toml"


def load_config(path: Path | str | None = None) -> dict[str, Any]:
    p = Path(path) if path else DEFAULT_CONFIG_PATH
    if not p.exists():
        return {}
    with p.open("rb") as f:
        return tomllib.load(f)


def resolve_path(p: str | Path, base: Path = _SCRIPT_DIR) -> Path:
    pp = Path(p)
    return pp if pp.is_absolute() else (base / pp).resolve()


def pick(
    cli: argparse.Namespace,
    attr: str | None,
    cfg: dict[str, Any],
    section: str,
    key: str,
    default: Any,
) -> Any:
    if attr is not None:
        cli_val = getattr(cli, attr, None)
        if cli_val is not None:
            return cli_val
    sect = cfg.get(section, {})
    if key in sect:
        return sect[key]
    return default

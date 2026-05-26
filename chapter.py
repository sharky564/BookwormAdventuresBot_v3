_BASE = [
    1, 1.25, 1.25, 1, 1, 1.25, 1, 1.25, 1, 1.75, 1.75, 1, 1.25,
    1, 1, 1.25, 2.75, 1, 1, 1, 1, 1.5, 1.5, 2, 1.5, 2
]  # fmt: skip


def _with(base: list[float], **overrides: float) -> list[float]:
    out = list(base)
    for k, v in overrides.items():
        out[ord(k.upper()) - ord("A")] = v
    return out


# After chapter 1.1: X, Y, Z buffed
_XYZ_25 = _with(_BASE, X=2.5, Y=2.5, Z=2.5)

# After chapter 1.7: X, Y, Z buffed further
_XYZ_3 = _with(_BASE, X=3.0, Y=3.0, Z=3.0)

# After chapter 2.5: R buffed too
_XYZ_3_R_2 = _with(_XYZ_3, R=2.0)


CHAPTERS: dict[str, dict] = {
    "1.1": dict(letter_points=_BASE, prescramble=True, gems_enabled=False),
    "1.2": dict(letter_points=_XYZ_25, prescramble=True, gems_enabled=False),
    "1.4": dict(letter_points=_XYZ_25, prescramble=False, gems_enabled=False),
    "1.6": dict(letter_points=_XYZ_25, prescramble=False, gems_enabled=True),
    "1.8": dict(letter_points=_XYZ_3, prescramble=False, gems_enabled=True),
    "2.6": dict(letter_points=_XYZ_3_R_2, prescramble=False, gems_enabled=True),
}


def list_chapters() -> list[str]:
    return sorted(CHAPTERS.keys(), key=lambda s: tuple(int(x) for x in s.split(".")))


if __name__ == "__main__":
    for k in list_chapters():
        cfg = CHAPTERS[k]
        print(
            f"{k}: gems={cfg['gems_enabled']} prescramble={cfg['prescramble']} "
            f"X={cfg['letter_points'][23]} Y={cfg['letter_points'][24]} "
            f"Z={cfg['letter_points'][25]} R={cfg['letter_points'][17]}"
        )

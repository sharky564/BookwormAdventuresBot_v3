"""Tune the kill-aware lookahead shape (kill_gamma, kill_margin) against
honest-play turn count, using the in-C++ `simulate` op for speed.

Honest play, no rack resets. The lookahead future is shaped by gamma/margin,
and that future only matters on NON-terminal multi-turn fights (where killing
enemy K sets up enemy K+1). We sweep gamma/margin over chapters containing
such fights and report total mean turns.
"""
from __future__ import annotations
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import Engine
import progress as P
import chapter as C


def chapter_spec(book, chap):
    p0 = P.Progress(book, chap, 0)
    preset = P.active_chapter_preset(p0)
    cfg_src = C.CHAPTERS[preset]
    config = {
        "gems_enabled": bool(cfg_src["gems_enabled"]),
        "prescramble": bool(cfg_src["prescramble"]),
        "letter_points": list(cfg_src["letter_points"]),
    }
    enemies = []
    for e in range(len(P.MONSTER_TABLE[book][chap])):
        p = P.Progress(book, chap, e)
        hp, armour = P.monster_at(p)
        wk_cat, wk_boost = P.active_weakness(p)
        enemies.append({
            "hp": hp * 4, "armour": int(armour), "power": float(P.power_at(p)),
            "weakness_cat": int(wk_cat), "weakness_boost": float(wk_boost),
            "base_damage_bonus": int(P.base_damage_bonus_at(p)),
            "treasure": bool(P.is_treasure_equipped(p)),
            "terminal": bool(P.is_terminal_kill(p)),
        })
    return config, enemies


def run_sim(eng, config, enemies, *, gamma, margin, seeds, max_sims, horizon, base_seed,
            future_metric="kill", prefilter_k=None):
    msg = {"op": "simulate", "config": config, "enemies": enemies, "seeds": seeds,
           "max_sims": max_sims, "horizon": horizon, "kill_gamma": gamma,
           "kill_margin": margin, "base_seed": base_seed, "turn_cap": 50,
           "future_metric": future_metric}
    if prefilter_k is not None:
        msg["prefilter_k"] = prefilter_k
    r = eng._send(msg)
    return float(r.get("mean_turns", 0.0)), float(r.get("fail_rate", 0.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=60)
    ap.add_argument("--max-sims", type=int, default=60)
    ap.add_argument("--horizon", type=int, default=2)
    ap.add_argument("--exe", default="solver/solver_sim")
    ap.add_argument("--dict", default="game_data/ba1-dictionary.txt")
    ap.add_argument("--chapters", default="1.9,2.0,2.6,2.9")
    ap.add_argument("--base-seed", type=int, default=1)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--ab", action="store_true",
                    help="A/B the future metric: plain sum-of-damage vs kill-aware")
    ap.add_argument("--ab-gamma", type=float, default=1.0,
                    help="gamma for the kill-aware arm of the A/B")
    ap.add_argument("--ab-margin", type=float, default=32.0,
                    help="margin for the kill-aware arm of the A/B")
    ap.add_argument("--prefilter-k", type=int, default=None,
                    help="MC-prefilter K for sim_pick_best (0/None = off, exact; "
                         "e.g. 48 = ~10x faster sweeps with a small consistent "
                         "policy bias -- re-check top cells with it off)")
    ap.add_argument("--engine", choices=["dfs", "scan"], default=None,
                    help="search engine (scan = hybrid anagram-class scan; "
                         "re-tune gamma/margin when switching)")
    args = ap.parse_args()
    eng = Engine(os.path.abspath(args.exe), os.path.abspath(args.dict))
    if args.engine:
        eng._send({"op": "config", "engine": args.engine})
    chapters = []
    for tok in args.chapters.split(","):
        b, c = tok.split("."); chapters.append((int(b), int(c)))
    specs = [(b, c, *chapter_spec(b, c)) for (b, c) in chapters]

    if args.ab:
        print(f"A/B future metric | chapters {[f'{b+1}.{c+1}' for b,c in chapters]} | "
              f"seeds={args.seeds} max_sims={args.max_sims} horizon={args.horizon}")
        print("  plain = sum-of-damage rollout (no gamma/margin)")
        print(f"  kill  = kill-aware (gamma={args.ab_gamma}, margin={args.ab_margin})\n")
        totals = {"plain": 0.0, "kill": 0.0}
        for (b, c, cfg, enemies) in specs:
            row = {}
            for metric in ("plain", "kill"):
                mt, fr = run_sim(eng, cfg, enemies, gamma=args.ab_gamma,
                                 margin=args.ab_margin, seeds=args.seeds,
                                 max_sims=args.max_sims, horizon=args.horizon,
                                 base_seed=args.base_seed, future_metric=metric,
                                 prefilter_k=args.prefilter_k)
                row[metric] = (mt, fr)
                totals[metric] += mt
            pm, pf = row["plain"]; km, kf = row["kill"]
            winner = "plain" if pm < km else ("kill" if km < pm else "tie")
            print(f"  {b+1}.{c+1}: plain={pm:6.2f} (fail {100*pf:.0f}%)  "
                  f"kill={km:6.2f} (fail {100*kf:.0f}%)  "
                  f"diff={km-pm:+.2f}  -> {winner}")
        eng.close()
        print(f"\n  TOTAL: plain={totals['plain']:.2f}  kill={totals['kill']:.2f}  "
              f"diff(kill-plain)={totals['kill']-totals['plain']:+.2f}")
        if totals["plain"] < totals["kill"]:
            print("  => PLAIN future wins: the kill-aware metric is not earning "
                  "its complexity here; consider switching the future term to "
                  "plain sum-of-damage and dropping gamma/margin.")
        elif totals["kill"] < totals["plain"]:
            print("  => KILL-AWARE future wins: keep the kill metric.")
        else:
            print("  => TIE: prefer plain for simplicity.")
        return

    if args.quick:
        gammas = [0.5, 0.7, 0.9]; margins = [4.0, 8.0]
    else:
        gammas = [0.4, 0.55, 0.7, 0.82, 0.92]; margins = [3.0, 6.0, 9.0, 14.0]
    print(f"Chapters {[f'{b+1}.{c+1}' for b,c in chapters]} | seeds={args.seeds} "
          f"max_sims={args.max_sims} horizon={args.horizon}")
    print(f"Grid: gamma={gammas} margin={margins}\n")
    results = {}
    for g in gammas:
        for m in margins:
            total = 0.0; fails = []; per = []
            for (b, c, cfg, enemies) in specs:
                mt, fr = run_sim(eng, cfg, enemies, gamma=g, margin=m, seeds=args.seeds,
                                 max_sims=args.max_sims, horizon=args.horizon, base_seed=args.base_seed,
                                 prefilter_k=args.prefilter_k)
                total += mt; fails.append(fr); per.append(mt)
            avg_fail = sum(fails) / len(fails); results[(g, m)] = (total, avg_fail)
            ch_str = " ".join(f"{x:5.1f}" for x in per)
            print(f"  gamma={g:<4} margin={m:<4}: total={total:6.2f}  fail={100*avg_fail:4.1f}%  [{ch_str}]")
    eng.close()
    ranked = sorted(results.items(), key=lambda kv: (kv[1][0], kv[1][1]))
    print("\nTop 5:")
    for (g, m), (t, f) in ranked[:5]:
        print(f"  gamma={g} margin={m}: total={t:.2f} fail={100*f:.1f}%")
    base = results.get((0.7, 9.0))
    bestkey, (bt, bf) = ranked[0]
    print(f"\nBest: gamma={bestkey[0]} margin={bestkey[1]}  total={bt:.2f}")
    if base:
        print(f"Default-ish (0.7, 9.0): total={base[0]:.2f}")
        if base[0] > 0:
            print(f"Improvement: {base[0]-bt:+.2f} turns ({100*(base[0]-bt)/base[0]:+.1f}%)")


if __name__ == "__main__":
    main()
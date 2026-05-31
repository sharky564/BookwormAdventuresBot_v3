from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


GEMS = [
    "none",
    "amethyst",
    "emerald",
    "sapphire",
    "garnet",
    "ruby",
    "crystal",
    "diamond",
]
STATUSES = ["normal", "smashed", "locked", "plague"]


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="Path to rack image (PNG)")
    ap.add_argument("--rack-offset-x", type=int, default=0)
    ap.add_argument("--rack-offset-y", type=int, default=0)
    ap.add_argument("--tile-size-x", type=int, default=50)
    ap.add_argument("--tile-size-y", type=int, default=51)
    ap.add_argument(
        "--description", default="", help="Free-text description saved with the fixture"
    )
    ap.add_argument(
        "--output",
        default=None,
        help="Output JSON path. Defaults to image's stem + .json",
    )
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    import tkinter as tk
    from PIL import Image, ImageTk

    img_path = Path(args.image)
    out_path = Path(args.output) if args.output else img_path.with_suffix(".json")

    img = Image.open(img_path).convert("RGB")
    tile_w, tile_h = args.tile_size_x, args.tile_size_y
    ox, oy = args.rack_offset_x, args.rack_offset_y

    tiles: list[dict] = []
    if out_path.exists():
        existing = json.loads(out_path.read_text())
        tiles = existing.get("tiles", [])
        by_pos = {(t["row"], t["col"]): t for t in tiles}
        tiles = []
        for row in range(4):
            for col in range(4):
                tiles.append(
                    by_pos.get(
                        (row, col),
                        {
                            "row": row,
                            "col": col,
                            "letter": "",
                            "gem": "none",
                            "status": "normal",
                        },
                    )
                )
    else:
        for row in range(4):
            for col in range(4):
                tiles.append(
                    {
                        "row": row,
                        "col": col,
                        "letter": "",
                        "gem": "none",
                        "status": "normal",
                    }
                )

    state = {"idx": 0}

    root = tk.Tk()
    root.title(f"Label fixture: {img_path.name}")
    root.configure(bg="#1c1c1c")

    preview = tk.Label(root, bg="#1c1c1c")
    preview.grid(row=0, column=0, rowspan=8, padx=10, pady=10)

    pos_label = tk.Label(
        root, text="", font=("Helvetica", 12, "bold"), bg="#1c1c1c", fg="white"
    )
    pos_label.grid(row=0, column=1, columnspan=2, sticky="w", padx=10)

    tk.Label(root, text="Letter:", bg="#1c1c1c", fg="white").grid(
        row=1, column=1, sticky="e", padx=4
    )
    letter_var = tk.StringVar()
    letter_entry = tk.Entry(
        root, textvariable=letter_var, width=4, font=("Consolas", 18)
    )
    letter_entry.grid(row=1, column=2, sticky="w")
    letter_entry.focus_set()

    tk.Label(root, text="Gem:", bg="#1c1c1c", fg="white").grid(
        row=2, column=1, sticky="ne", padx=4, pady=(8, 0)
    )
    gem_var = tk.StringVar(value="none")
    gem_frame = tk.Frame(root, bg="#1c1c1c")
    gem_frame.grid(row=2, column=2, sticky="w", pady=(8, 0))
    for i, name in enumerate(GEMS):
        rb = tk.Radiobutton(
            gem_frame,
            text=name,
            variable=gem_var,
            value=name,
            bg="#1c1c1c",
            fg="white",
            selectcolor="#444",
            activebackground="#2a2a2a",
            activeforeground="white",
            anchor="w",
            width=10,
        )
        rb.grid(row=i // 2, column=i % 2, sticky="w")

    tk.Label(root, text="Status:", bg="#1c1c1c", fg="white").grid(
        row=3, column=1, sticky="ne", padx=4, pady=(8, 0)
    )
    status_var = tk.StringVar(value="normal")
    status_frame = tk.Frame(root, bg="#1c1c1c")
    status_frame.grid(row=3, column=2, sticky="w", pady=(8, 0))
    for i, name in enumerate(STATUSES):
        rb = tk.Radiobutton(
            status_frame,
            text=name,
            variable=status_var,
            value=name,
            bg="#1c1c1c",
            fg="white",
            selectcolor="#444",
            activebackground="#2a2a2a",
            activeforeground="white",
            anchor="w",
            width=10,
        )
        rb.grid(row=i // 2, column=i % 2, sticky="w")

    overview = tk.Frame(root, bg="#1c1c1c")
    overview.grid(row=4, column=1, columnspan=2, pady=(10, 0))
    overview_labels: list[tk.Label] = []
    for r in range(4):
        for c in range(4):
            lbl = tk.Label(
                overview,
                text="?",
                width=3,
                height=1,
                bg="#2c2c2c",
                fg="white",
                borderwidth=1,
                relief="solid",
                font=("Consolas", 10),
            )
            lbl.grid(row=r, column=c, padx=1, pady=1)
            overview_labels.append(lbl)

    status_msg = tk.Label(root, text="", bg="#1c1c1c", fg="#8f8")
    status_msg.grid(row=5, column=1, columnspan=2, sticky="w", padx=10, pady=(8, 0))

    def commit_current():
        """Read UI state into tiles[state['idx']]."""
        t = tiles[state["idx"]]
        t["letter"] = letter_var.get().strip().upper()[:1] or ""
        t["gem"] = gem_var.get()
        t["status"] = status_var.get()

    def load_current():
        t = tiles[state["idx"]]
        pos_label.config(
            text=f"Tile [{t['row']}, {t['col']}]  (idx {state['idx'] + 1}/16)"
        )
        letter_var.set(t["letter"])
        gem_var.set(t["gem"])
        status_var.set(t["status"])

        row, col = t["row"], t["col"]
        tile = img.crop(
            (
                ox + col * tile_w,
                oy + row * tile_h,
                ox + (col + 1) * tile_w,
                oy + (row + 1) * tile_h,
            )
        )
        big = tile.resize((tile.size[0] * 4, tile.size[1] * 4), Image.NEAREST)
        tk_img = ImageTk.PhotoImage(big, master=root)
        preview.config(image=tk_img)
        preview.image = tk_img

        letter_entry.focus_set()
        letter_entry.select_range(0, "end")
        update_overview()

    def update_overview():
        for i, t in enumerate(tiles):
            text = t["letter"] or "?"
            if t["status"] != "normal":
                text = t["status"][0].upper()
            bg = "#2c2c2c"
            if i == state["idx"]:
                bg = "#fa3"
            overview_labels[i].config(text=text, bg=bg)

    def goto(idx: int):
        commit_current()
        state["idx"] = idx % 16
        load_current()

    def next_tile(e=None):
        goto(state["idx"] + 1)

    def prev_tile(e=None):
        goto(state["idx"] - 1)

    def save_quit(e=None):
        commit_current()
        payload = {
            "description": args.description,
            "image": img_path.name,
            "rack_offset_x": ox,
            "rack_offset_y": oy,
            "tile_size_x": tile_w,
            "tile_size_y": tile_h,
            "tiles": tiles,
        }
        out_path.write_text(json.dumps(payload, indent=2))
        status_msg.config(text=f"Saved: {out_path}")
        root.after(400, root.destroy)

    root.bind("<Tab>", next_tile)
    root.bind("<Shift-Tab>", prev_tile)
    root.bind("<Return>", save_quit)

    def on_letter_key(e):
        if not e.keysym.isalpha() or len(e.keysym) != 1:
            return
        tiles[state["idx"]]["letter"] = e.keysym.upper()
        tiles[state["idx"]]["gem"] = gem_var.get()
        tiles[state["idx"]]["status"] = status_var.get()
        state["idx"] = (state["idx"] + 1) % 16
        load_current()
        return "break"

    root.bind("<KeyPress>", on_letter_key)

    load_current()
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

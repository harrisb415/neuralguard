"""Design + verify the sidebar nav icon set, then emit WinUI PathIcon Data
strings from the exact same coordinates used for the Pillow preview (no manual
transcription, so the preview and the shipped icon can't drift apart).

Each icon is a set of FILLED subpaths (matching the app's brand-mark style -
solid silhouettes, not stroked line-art) on a 24x24 grid. PathIcon fills with
Foreground and has no separate Stroke, so every shape here is expressed as a
filled polygon (or a filled circle via two arcs) rather than an open line.

Run:  python scripts/gen_nav_icons.py
Prints each icon's Path.Data string and writes a visual contact sheet PNG for
review before anything is pasted into MainWindow.xaml.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

PREVIEW_OUT = Path(__file__).resolve().parent.parent / "scratchpad_nav_icons.png"
FG = (232, 236, 244, 255)


def circle_points(cx, cy, r, n=24):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def rect_points(x, y, w, h):
    return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]


# Each icon: list of subpaths (each a list of (x,y) points), all filled.
ICONS = {
    "live": [circle_points(12, 12, 6.5)],
    "rules": [rect_points(3, 5, 18, 3), rect_points(3, 10.5, 14, 3), rect_points(3, 16, 10, 3)],
    "habits": [  # circular refresh arrow: thick arc band + arrowhead
        [(12 + r * math.cos(a), 12 + r * math.sin(a))
         for r in (9, 6) for a in ([math.radians(d) for d in range(-40, 220, 20)] if r == 9
                                    else [math.radians(d) for d in range(220, -40, -20)])],
        [(20, 6), (24 - 1, 10), (15, 10)],
    ],
    # Gap between squares must survive downscaling to ~16px (a 2-unit gap on a
    # 24-unit grid is under a pixel at that size and disappears - verified by
    # rendering at true size; 4 units holds up).
    "apps": [rect_points(2, 2, 8, 8), rect_points(14, 2, 8, 8), rect_points(2, 14, 8, 8), rect_points(14, 14, 8, 8)],
    "history": [circle_points(12, 12, 9),  # solid disk + a thin wedge notch cut
                [(12, 12)] + [(12 + 9.5 * math.cos(math.radians(d)), 12 + 9.5 * math.sin(math.radians(d)))
                              for d in range(-100, -50, 5)]],
    "flows": [  # three right-pointing chevrons (flow direction)
        [(2, 4), (8, 4), (14, 12), (8, 20), (2, 20), (8, 12)],
        [(11, 4), (17, 4), (23, 12), (17, 20), (11, 20), (17, 12)],
    ],
    "flags": [rect_points(4, 2, 2.4, 20), [(6.4, 3), (20, 7), (6.4, 11)]],
    "baseline": [rect_points(3, 16, 4, 5), rect_points(10, 10, 4, 11), rect_points(17, 4, 4, 17)],
    "feedback": [  # speech bubble: rounded rect + tail
        rect_points(3, 3, 18, 13), [(7, 16), (13, 16), (7, 21)],
    ],
    # inbound: an arrow entering a wall ("->|") = traffic arriving at this machine.
    "inbound": [
        rect_points(19, 3, 3, 18),                 # the wall (your machine)
        rect_points(2, 10.5, 9, 3),                # shaft
        [(10, 6.5), (17, 12), (10, 17.5)],         # arrowhead pointing in
    ],
    # Page with a folded corner (classic "document" motif) - a single big
    # dog-ear notch survives small-size rendering far better than thin text
    # lines, which turned out to be sub-pixel and vanished at 16px.
    "digest": [rect_points(4, 2, 16, 20), [(14, 2), (20, 2), (20, 8)]],
    "settings": [  # gear: hub circle + 8 radial teeth
        circle_points(12, 12, 5.5),
        *[[(12 + 5 * math.cos(a) + 2.2 * math.cos(a + math.pi / 2), 12 + 5 * math.sin(a) + 2.2 * math.sin(a + math.pi / 2)),
           (12 + 5 * math.cos(a) - 2.2 * math.cos(a + math.pi / 2), 12 + 5 * math.sin(a) - 2.2 * math.sin(a + math.pi / 2)),
           (12 + 8.6 * math.cos(a) - 2.2 * math.cos(a + math.pi / 2), 12 + 8.6 * math.sin(a) - 2.2 * math.sin(a + math.pi / 2)),
           (12 + 8.6 * math.cos(a) + 2.2 * math.cos(a + math.pi / 2), 12 + 8.6 * math.sin(a) + 2.2 * math.sin(a + math.pi / 2))]
          for a in [math.radians(d) for d in range(0, 360, 45)]],
    ],
}

ORDER = ["live", "rules", "habits", "apps", "history", "flows", "flags", "baseline", "inbound",
         "feedback", "digest", "settings"]

# Icons whose extra subpaths are holes cut from an earlier one (evenodd / "F0")
# rather than additional solid shapes merged in (nonzero / "F1", the default -
# used e.g. by settings' teeth, which deliberately overlap the hub in the same
# fill so they read as one solid gear rather than punching a ring into it).
EVENODD = {"history", "digest"}


def fmt(n):
    s = f"{n:.2f}".rstrip("0").rstrip(".")
    return s if s else "0"


def to_path_data(name, subpaths):
    parts = []
    for sp in subpaths:
        if not sp:
            continue
        parts.append(f"M{fmt(sp[0][0])},{fmt(sp[0][1])}" + "".join(f"L{fmt(x)},{fmt(y)}" for x, y in sp[1:]) + "Z")
    rule = "F0" if name in EVENODD else "F1"
    return f"{rule} " + " ".join(parts)


def render_preview():
    cell = 64
    cols = 6
    rows = (len(ORDER) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell, rows * cell), (18, 19, 23, 255))
    bg = (18, 19, 23, 255)
    for i, name in enumerate(ORDER):
        x0, y0 = (i % cols) * cell, (i // cols) * cell
        scale = 2.2
        off = (x0 + cell / 2 - 12 * scale, y0 + 8)
        d = ImageDraw.Draw(sheet)
        subpaths = ICONS[name]
        evenodd = name in EVENODD
        for idx, sp in enumerate(subpaths):
            pts = [(off[0] + x * scale, off[1] + y * scale) for x, y in sp]
            # Approximate evenodd for preview purposes: subpath 0 is the solid
            # base, later subpaths in an evenodd icon are holes (background-
            # colored); a nonzero icon just merges every subpath in foreground.
            fill = bg if (evenodd and idx > 0) else FG
            d.polygon(pts, fill=fill)
        d.text((x0 + 4, y0 + cell - 14), name, fill=(150, 155, 165, 255))
    sheet.save(PREVIEW_OUT)


def main():
    render_preview()
    print(f"Preview written to {PREVIEW_OUT}\n")
    for name in ORDER:
        print(f'{name}: Data="{to_path_data(name, ICONS[name])}"')


if __name__ == "__main__":
    main()

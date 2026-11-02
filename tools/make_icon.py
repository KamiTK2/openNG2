"""Render the openNG2 icon ("oNG2") to assets/openNG2.ico (+ a preview PNG).

Usage: python -I tools/make_icon.py
Dark rounded square, a red sword slash, "o" in red and "NG2" in white (Segoe UI Black).
Each size is drawn at 8x and downsampled, so small sizes stay crisp.
"""
import os
import shutil

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets")
FONT = "C:/Windows/Fonts/seguibl.ttf"
RED = (205, 34, 34, 255)
RED_DARK = (120, 14, 14, 255)
WHITE = (244, 244, 246, 255)
BG_TOP = (30, 30, 36)
BG_BOTTOM = (12, 12, 15)


def render(size):
    ss = 8
    s = size * ss
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # Background: rounded square with a vertical gradient.
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (s, y)], fill=c)
    mask = Image.new("L", (s, s), 0)
    radius = int(s * 0.2)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=radius, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)

    # Sword slash: a long tapered diagonal from bottom-left to top-right.
    slash = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sd = ImageDraw.Draw(slash)
    w = s * 0.075
    x0, y0, x1, y1 = s * 0.06, s * 0.94, s * 0.94, s * 0.10
    sd.polygon([(x0, y0), (x1 - w * 0.1, y1), (x1 + w * 0.05, y1 + w * 0.6), (x0 + w * 1.6, y0)], fill=RED_DARK)
    sd.polygon([(x0 + w * 0.5, y0), (x1, y1 + w * 0.15), (x1, y1 + w * 0.35), (x0 + w * 1.1, y0)], fill=RED)
    slash_masked = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    slash_masked.paste(slash, (0, 0), Image.composite(slash.split()[3], Image.new("L", (s, s), 0), mask))
    img = Image.alpha_composite(img, slash_masked)
    d = ImageDraw.Draw(img)

    # Text: tighter and larger at small sizes.
    small = size <= 24
    font = ImageFont.truetype(FONT, int(s * (0.46 if small else 0.32)))
    parts = [("o", RED), ("NG2", WHITE)] if not small else [("NG2", WHITE)]
    widths = [d.textlength(t, font=font) for t, _ in parts]
    gap = -s * 0.01
    total = sum(widths) + gap * (len(parts) - 1)
    bbox = d.textbbox((0, 0), "NG2", font=font)
    th = bbox[3] - bbox[1]
    x = (s - total) / 2
    y = (s - th) / 2 - bbox[1] - s * (0.0 if small else 0.02)
    outline = max(1, int(s * 0.018))
    for (t, color), tw in zip(parts, widths):
        d.text((x, y), t, font=font, fill=color, stroke_width=outline, stroke_fill=(10, 10, 12, 255))
        x += tw + gap
    if small:
        # Keep the red "o" as a dot in the corner so the brand colour survives.
        r = s * 0.11
        d.ellipse([s * 0.06, s * 0.06, s * 0.06 + 2 * r, s * 0.06 + 2 * r], fill=RED)
    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [render(n) for n in sizes]
    images[-1].save(os.path.join(OUT, "openNG2.ico"), sizes=[(n, n) for n in sizes],
                    append_images=images[:-1])
    # Preview strip for review.
    strip = Image.new("RGBA", (sum(sizes) + 20 * len(sizes), 276), (40, 40, 44, 255))
    x = 10
    for im in images:
        strip.paste(im, (x, (276 - im.height) // 2), im)
        x += im.width + 20
    strip.save(os.path.join(OUT, "openNG2-preview.png"))
    images[-1].save(os.path.join(OUT, "openNG2-256.png"))
    # The game project embeds it too (shipped in the release payload).
    shutil.copyfile(os.path.join(OUT, "openNG2.ico"), os.path.join(ROOT, "project", "ng2", "src", "openNG2.ico"))
    print("wrote", os.path.join(OUT, "openNG2.ico"))


if __name__ == "__main__":
    main()

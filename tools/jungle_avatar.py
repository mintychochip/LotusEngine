#!/usr/bin/env python3
"""Procedural explorer avatar: one atlas-mapped GLB plus its 2K texture atlas.

The engine draws one material per entity, so the whole character is a single
mesh whose UVs index a packed atlas. That buys painted face detail, cloth
weave, seams, pockets, boot laces and pack hardware without a second draw.

Local frame: origin at the feet, +Y up, +Z forward. The scene parents this mesh
to the `player` entity, which turns it to face down the trail.
"""

import json
import math
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

PI = math.pi
TAU = 2.0 * PI
SIZE = 2048

# Pixel rectangles in the atlas. v = 0 is the top of the image, matching the
# engine's sampler (calibrated with a signed test texture).
RECTS = {
    "face": (0, 0, 1024, 1024),
    "skin": (1024, 0, 512, 512),
    "leather": (1536, 0, 512, 512),
    "shirt": (1024, 512, 1024, 512),
    "pants": (0, 1024, 1024, 512),
    "canvas": (1024, 1024, 512, 512),
    "shirt_plain": (1536, 1024, 512, 512),
    "scarf": (1024, 1536, 256, 256),
    "sole": (0, 1536, 256, 256),
    "bedroll": (256, 1536, 512, 256),
    "boot": (768, 1536, 256, 256),
    "pants_plain": (1024, 1792, 256, 256),
    "metal": (1280, 1792, 256, 256),
    "hair": (1536, 1792, 256, 256),
    "dirt": (1792, 1792, 256, 256),
    "canvas_plain": (1280, 1536, 256, 256),
}


def rect_uv(name, u, v):
    x, y, w, h = RECTS[name]
    return ((x + u * w) / SIZE, (y + v * h) / SIZE)


def smooth_noise(w, h, freq, rng):
    small = rng.random((freq, freq)).astype(np.float32)
    image = Image.fromarray((small * 255).astype(np.uint8), "L")
    image = image.resize((w, h), Image.BICUBIC)
    return np.asarray(image).astype(np.float32) / 255.0


def weave(w, h, cells):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    pattern = np.sin(xx * TAU * cells / w) * np.sin(yy * TAU * cells / h)
    return pattern


def paint_base(tex, name, base, dark, light, rng, freq=8, weave_cells=0, weave_amount=0.0):
    x, y, w, h = RECTS[name]
    n = smooth_noise(w, h, freq, rng)
    lo = np.asarray(dark, dtype=np.float32)
    hi = np.asarray(light, dtype=np.float32)
    value = n[..., None]
    rgb = lo * (1.0 - value) + hi * value
    rgb = rgb * np.asarray(base, dtype=np.float32)[None, None, :]
    if weave_cells:
        rgb *= (1.0 + weave(w, h, weave_cells)[..., None] * weave_amount)
    tex[y:y+h, x:x+w] = np.clip(rgb, 0.0, 1.0)


def build_atlas(path, face_source=None):
    rng = np.random.default_rng(11)
    tex = np.zeros((SIZE, SIZE, 3), dtype=np.float32)
    tex[:] = (0.04, 0.04, 0.04)

    paint_base(tex, "skin", (1.0, 1.0, 1.0), (0.45, 0.32, 0.245), (0.55, 0.40, 0.315), rng, freq=220)
    paint_base(tex, "shirt", (1.0, 1.0, 1.0), (0.40, 0.31, 0.19), (0.52, 0.41, 0.25), rng, freq=150, weave_cells=520, weave_amount=0.014)
    paint_base(tex, "shirt_plain", (1.0, 1.0, 1.0), (0.40, 0.31, 0.19), (0.52, 0.41, 0.25), rng, freq=170, weave_cells=320, weave_amount=0.014)
    paint_base(tex, "pants", (1.0, 1.0, 1.0), (0.19, 0.16, 0.10), (0.28, 0.24, 0.15), rng, freq=150, weave_cells=360, weave_amount=0.014)
    paint_base(tex, "pants_plain", (1.0, 1.0, 1.0), (0.19, 0.16, 0.10), (0.28, 0.24, 0.15), rng, freq=170, weave_cells=240, weave_amount=0.014)
    paint_base(tex, "leather", (1.0, 1.0, 1.0), (0.11, 0.07, 0.04), (0.28, 0.18, 0.10), rng, freq=42)
    paint_base(tex, "canvas", (1.0, 1.0, 1.0), (0.26, 0.18, 0.11), (0.45, 0.33, 0.20), rng, freq=48, weave_cells=300, weave_amount=0.025)
    paint_base(tex, "canvas_plain", (1.0, 1.0, 1.0), (0.27, 0.19, 0.12), (0.44, 0.32, 0.20), rng, freq=120, weave_cells=320, weave_amount=0.020)
    paint_base(tex, "boot", (1.0, 1.0, 1.0), (0.075, 0.048, 0.030), (0.20, 0.13, 0.075), rng, freq=48)
    paint_base(tex, "sole", (1.0, 1.0, 1.0), (0.04, 0.04, 0.035), (0.10, 0.10, 0.09), rng, freq=20)
    paint_base(tex, "metal", (1.0, 1.0, 1.0), (0.22, 0.23, 0.22), (0.62, 0.64, 0.65), rng, freq=22)
    paint_base(tex, "scarf", (1.0, 1.0, 1.0), (0.20, 0.03, 0.025), (0.52, 0.08, 0.06), rng, freq=48, weave_cells=120, weave_amount=0.07)
    paint_base(tex, "bedroll", (1.0, 1.0, 1.0), (0.16, 0.18, 0.10), (0.34, 0.37, 0.22), rng, freq=48, weave_cells=260, weave_amount=0.025)
    paint_base(tex, "hair", (1.0, 1.0, 1.0), (0.035, 0.020, 0.012), (0.14, 0.085, 0.045), rng, freq=70)
    paint_base(tex, "dirt", (1.0, 1.0, 1.0), (0.08, 0.055, 0.035), (0.20, 0.14, 0.08), rng, freq=48)
    paint_base(tex, "face", (1.0, 1.0, 1.0), (0.45, 0.32, 0.245), (0.55, 0.40, 0.315), rng, freq=220)

    image = Image.fromarray(np.clip(tex * 255.0, 0, 255).astype(np.uint8), "RGB")
    draw = ImageDraw.Draw(image)

    def rect(name):
        return RECTS[name]

    # --- face: full spherical map, front centre at x=512 -------------------
    # A neutral, soft-skinned male face. The sphere UV wraps this cell 360
    # degrees, so the front occupies roughly x 300..724 and the hair lives at
    # the sides/back under the hat.
    fx, fy, fw, fh = rect("face")
    draw.rectangle([fx, fy, fx + fw, fy + fh], fill=(159, 115, 86))
    # Fine skin grain instead of large blotches.
    for _ in range(9000):
        x = rng.uniform(0, fw)
        y = rng.uniform(0, fh)
        r = rng.uniform(0.5, 1.8)
        shade = int(rng.uniform(-10, 10))
        draw.ellipse([fx + x - r, fy + y - r, fx + x + r, fy + y + r],
                     fill=(159 + shade, 115 + shade, 86 + shade))
    # Hair cap and narrow sideburns.
    hair = (43, 29, 20)
    draw.polygon(
        [(fx, fy), (fx + fw, fy), (fx + fw, fy + 165),
         (fx + 930, fy + 188), (fx + 820, fy + 110),
         (fx + 204, fy + 110), (fx + 94, fy + 188), (fx, fy + 165)],
        fill=hair,
    )
    draw.polygon([(fx, fy + 165), (fx + 104, fy + 205),
                  (fx + 86, fy + 470), (fx, fy + 520)], fill=hair)
    draw.polygon([(fx + fw, fy + 165), (fx + fw - 104, fy + 205),
                  (fx + fw - 86, fy + 470), (fx + fw, fy + 520)], fill=hair)
    for _ in range(700):
        x = rng.uniform(0, fw)
        y = rng.uniform(0, 230)
        if 190 < x < fw - 190:
            continue
        draw.line([(fx + x, fy + y), (fx + x + rng.uniform(-2, 2), fy + y + rng.uniform(8, 26))],
                  fill=(55, 37, 25), width=1)
    # Soft jaw/temple shading and cheek warmth.
    draw.ellipse([fx + 300, fy + 650, fx + 724, fy + 900], fill=(151, 107, 80))
    draw.ellipse([fx + 340, fy + 610, fx + 684, fy + 850], fill=(160, 115, 85))
    for ex in (392, 632):
        draw.ellipse([fx + ex - 70, fy + 540, fx + ex + 70, fy + 690],
                     fill=(168, 120, 91))
        draw.ellipse([fx + ex - 58, fy + 552, fx + ex + 58, fy + 676],
                     fill=(163, 116, 88))
    # Forehead highlight and brow ridge.
    draw.ellipse([fx + 402, fy + 214, fx + 622, fy + 402], fill=(169, 122, 92))
    draw.ellipse([fx + 420, fy + 230, fx + 604, fy + 380], fill=(172, 125, 95))
    # Eyes: almond sclera, brown iris, small pupil, upper lid and crease.
    for ex in (452, 572):
        draw.ellipse([fx + ex - 34, fy + 482, fx + ex + 34, fy + 526],
                     fill=(139, 100, 76))
        draw.ellipse([fx + ex - 27, fy + 493, fx + ex + 27, fy + 520],
                     fill=(211, 202, 188))
        draw.ellipse([fx + ex - 14, fy + 497, fx + ex + 14, fy + 520],
                     fill=(83, 56, 37))
        draw.ellipse([fx + ex - 7, fy + 501, fx + ex + 7, fy + 519],
                     fill=(20, 15, 11))
        draw.ellipse([fx + ex - 3, fy + 496, fx + ex + 1, fy + 500],
                     fill=(245, 240, 230))
        draw.arc([fx + ex - 29, fy + 486, fx + ex + 29, fy + 514],
                 195, 345, fill=(63, 42, 29), width=2)
        draw.arc([fx + ex - 27, fy + 508, fx + ex + 27, fy + 528],
                 15, 165, fill=(121, 85, 64), width=1)
        draw.arc([fx + ex - 34, fy + 470, fx + ex + 30, fy + 500],
                 200, 340, fill=(108, 78, 58), width=2)
        # Brow, tapered and arched.
        draw.line([(fx + ex - 40, fy + 466), (fx + ex - 8, fy + 451),
                   (fx + ex + 30, fy + 458)], fill=(57, 39, 27), width=5)
        draw.line([(fx + ex - 36, fy + 466), (fx + ex - 4, fy + 454)],
                  fill=(72, 50, 35), width=2)
    # Nose: bridge highlight, side shadows, tip and nostrils.
    draw.line([(fx + 514, fy + 508), (fx + 510, fy + 592)],
              fill=(176, 129, 99), width=3)
    draw.line([(fx + 492, fy + 540), (fx + 494, fy + 600)],
              fill=(133, 91, 67), width=5)
    draw.line([(fx + 534, fy + 540), (fx + 532, fy + 600)],
              fill=(139, 96, 71), width=5)
    draw.ellipse([fx + 486, fy + 584, fx + 538, fy + 616], fill=(168, 122, 93))
    draw.ellipse([fx + 480, fy + 596, fx + 500, fy + 612], fill=(96, 63, 46))
    draw.ellipse([fx + 524, fy + 596, fx + 544, fy + 612], fill=(96, 63, 46))
    # Mouth: muted lips with cupid's bow, chin shadow and crease.
    draw.polygon([(fx + 470, fy + 666), (fx + 496, fy + 654),
                  (fx + 512, fy + 660), (fx + 528, fy + 654),
                  (fx + 556, fy + 666), (fx + 532, fy + 674),
                  (fx + 512, fy + 671), (fx + 492, fy + 674)],
                 fill=(158, 99, 82))
    draw.polygon([(fx + 472, fy + 674), (fx + 512, fy + 672),
                  (fx + 552, fy + 675), (fx + 528, fy + 689),
                  (fx + 512, fy + 692), (fx + 496, fy + 689)],
                 fill=(171, 111, 94))
    draw.line([(fx + 480, fy + 672), (fx + 512, fy + 673), (fx + 546, fy + 672)],
              fill=(88, 45, 38), width=2)
    draw.arc([fx + 470, fy + 686, fx + 554, fy + 716], 200, 340,
             fill=(128, 86, 67), width=2)
    # Short stubble around the jaw and upper lip.
    for _ in range(2600):
        x = rng.normal(512, 135)
        y = rng.normal(748, 72)
        if 330 < x < 694 and 640 < y < 870:
            draw.point((fx + x, fy + y), fill=(104, 75, 58))
    draw.polygon([(fx + 456, fy + 662), (fx + 568, fy + 662),
                  (fx + 546, fy + 671), (fx + 512, fy + 666),
                  (fx + 478, fy + 671)], fill=(117, 84, 65))
    # Faint forehead lines and crow's feet; weather, not makeup.
    draw.line([(fx + 438, fy + 330), (fx + 588, fy + 326)],
              fill=(149, 106, 81), width=2)
    draw.line([(fx + 454, fy + 366), (fx + 574, fy + 363)],
              fill=(151, 108, 82), width=1)
    for ex in (392, 632):
        for step in range(3):
            draw.line([(fx + ex + (38 if ex < 512 else -38), fy + 510 + step * 12),
                       (fx + ex + (58 if ex < 512 else -58), fy + 504 + step * 14)],
                      fill=(139, 99, 76), width=1)
    # A little trail dirt on the cheekbones.
    for _ in range(60):
        x = rng.uniform(300, 724)
        y = rng.uniform(560, 760)
        r = rng.uniform(1, 5)
        draw.ellipse([fx + x - r, fy + y - r, fx + x + r, fy + y + r],
                     fill=(124, 91, 70))

    # --- optional photoreal face projection ------------------------------
    # The generated portrait is mapped to the front of the head atlas with an
    # affine landmark fit (eyes and mouth), then feathered into the painted
    # hair/skin fallback. Everything outside the oval is hair, so the back of
    # the head stays dark under the hat.
    if face_source and os.path.exists(face_source):
        try:
            source = Image.open(face_source).convert("RGB")
            # Source landmark coordinates in the 1254px portrait.
            src_eye_left, src_eye_right = 470.0, 760.0
            src_eye_y, src_mouth_y = 500.0, 790.0
            # Target landmarks on the 1024 face cell.
            dst_eye_left, dst_eye_right = 450.0, 574.0
            dst_eye_y, dst_mouth_y = 505.0, 665.0
            sx = (dst_eye_right - dst_eye_left) / (src_eye_right - src_eye_left)
            sy = (dst_mouth_y - dst_eye_y) / (src_mouth_y - src_eye_y)
            ox = dst_eye_left - src_eye_left * sx
            oy = dst_eye_y - src_eye_y * sy
            # Output -> input affine for PIL.
            affine = (
                1.0 / sx, 0.0, -ox / sx,
                0.0, 1.0 / sy, -oy / sy,
            )
            mapped = source.transform(
                (1024, 1024), Image.AFFINE, affine, Image.BICUBIC
            )
            # Base outside the oval: hair. The oval includes the face, ears and
            # hair; the bottom stops above the shirt collar.
            draw.rectangle([fx, fy, fx + fw, fy + fh], fill=(43, 29, 20))
            mask = Image.new("L", (1024, 1024), 0)
            mdraw = ImageDraw.Draw(mask)
            mdraw.ellipse([198, 142, 826, 888], fill=255)
            mask = mask.filter(ImageFilter.GaussianBlur(22))
            # Erase the lower neck/collar region so the shirt texture does not
            # bleed onto the jaw.
            lower = Image.new("L", (1024, 1024), 255)
            ImageDraw.Draw(lower).rectangle([0, 812, 1024, 1024], fill=0)
            lower = lower.filter(ImageFilter.GaussianBlur(18))
            mask = Image.composite(mask, Image.new("L", (1024, 1024), 0), lower)
            image.paste(mapped, (fx, fy), mask)
            # A few dark strands at the oval edge blend the photo into the
            # painted hair instead of leaving a clean egg outline.
            for _ in range(260):
                angle = rng.uniform(0, 2.0 * PI)
                side = rng.uniform(0.92, 1.06)
                x = 512 + math.cos(angle) * 314 * side
                y = 515 + math.sin(angle) * 372 * side
                if 100 < x < 924 and 110 < y < 900:
                    draw.line([(fx + x, fy + y),
                               (fx + x + rng.uniform(-6, 6),
                                fy + y + rng.uniform(10, 34))],
                              fill=(48, 32, 22), width=2)
        except Exception:
            pass

    # --- skin cell: pores, scratches, dirt --------------------------------
    sx, sy, sw, sh = rect("skin")
    for _ in range(1800):
        x = rng.uniform(0, sw)
        y = rng.uniform(0, sh)
        r = rng.uniform(0.5, 1.6)
        draw.ellipse([sx + x - r, sy + y - r, sx + x + r, sy + y + r],
                     fill=(148, 98, 70))
    for _ in range(20):
        x = rng.uniform(0, sw)
        y = rng.uniform(0, sh)
        draw.line([(sx + x, sy + y), (sx + x + rng.uniform(-20, 20),
                                      sy + y + rng.uniform(3, 18))],
                  fill=(124, 80, 58), width=1)

    # --- shirt wrap: front placket, pockets, seams ------------------------
    tx, ty, tw, th = rect("shirt")
    # Cloth folds and weave shadows.
    for _ in range(26):
        yy = rng.uniform(0, th)
        draw.line([(tx, ty + yy), (tx + tw, ty + yy + rng.uniform(-4, 4))],
                  fill=(96, 86, 56), width=1)
    # Center placket and buttons.
    draw.rectangle([tx + 480, ty + 40, tx + 544, ty + th - 20], fill=(108, 98, 62))
    draw.line([(tx + 512, ty + 40), (tx + 512, ty + th - 20)], fill=(76, 66, 42), width=3)
    for by in (120, 210, 300, 390):
        draw.ellipse([tx + 498, ty + by, tx + 526, ty + by + 28],
                     fill=(74, 58, 34), outline=(45, 35, 22), width=2)
    # Chest pockets and flaps.
    for px in (300, 620):
        draw.rectangle([tx + px, ty + 150, tx + px + 110, ty + 280],
                       fill=(122, 110, 72), outline=(76, 66, 42), width=3)
        draw.rectangle([tx + px - 4, ty + 140, tx + px + 114, ty + 178],
                       fill=(104, 92, 60), outline=(66, 56, 36), width=2)
        draw.line([(tx + px + 55, ty + 145), (tx + px + 55, ty + 175)],
                  fill=(56, 46, 28), width=2)
    # Collar shading and side seams.
    draw.line([(tx + 250, ty + 28), (tx + 774, ty + 28)], fill=(70, 60, 38), width=5)
    draw.line([(tx + 256, ty), (tx + 256, ty + th)], fill=(84, 74, 46), width=2)
    draw.line([(tx + 768, ty), (tx + 768, ty + th)], fill=(84, 74, 46), width=2)
    # Back seam.
    draw.line([(tx + 30, ty), (tx + 30, ty + th)], fill=(80, 70, 45), width=2)
    # Wear and mud.
    for _ in range(80):
        x = rng.uniform(0, tw)
        y = rng.uniform(0, th)
        r = rng.uniform(1, 6)
        draw.ellipse([tx + x - r, ty + y - r, tx + x + r, ty + y + r],
                     fill=(88, 76, 48))

    # --- pants wrap: fly, cargo pockets, wrinkles -------------------------
    px, py, pw, ph = rect("pants")
    draw.line([(px + 512, py + 30), (px + 512, py + 200)], fill=(48, 52, 28), width=5)
    draw.arc([px + 430, py + 40, px + 594, py + 200], 0, 90, fill=(48, 52, 28), width=3)
    for bx in (250, 700):
        draw.rectangle([px + bx, py + 130, px + bx + 90, py + 250],
                       fill=(58, 62, 32), outline=(38, 42, 22), width=3)
        draw.rectangle([px + bx - 3, py + 120, px + bx + 93, py + 148],
                       fill=(50, 54, 28), outline=(34, 38, 20), width=2)
    for _ in range(80):
        x = rng.uniform(0, pw)
        y = rng.uniform(0, ph)
        draw.line([(px + x, py + y), (px + x + rng.uniform(-20, 20),
                                      py + y + rng.uniform(8, 26))],
                  fill=(44, 48, 25), width=1)
    for _ in range(90):
        x = rng.uniform(0, pw)
        y = rng.uniform(ph * 0.7, ph)
        r = rng.uniform(1, 5)
        draw.ellipse([px + x - r, py + y - r, px + x + r, py + y + r],
                     fill=(50, 45, 26))

    # --- plain pants and shirt seams --------------------------------------
    qx, qy, qw, qh = rect("pants_plain")
    for _ in range(60):
        x = rng.uniform(0, qw)
        y = rng.uniform(0, qh)
        draw.line([(qx + x, qy + y), (qx + x + rng.uniform(-15, 15),
                                      qy + y + rng.uniform(8, 22))],
                  fill=(42, 46, 24), width=1)
    sx2, sy2, sw2, sh2 = rect("shirt_plain")
    for _ in range(50):
        x = rng.uniform(0, sw2)
        y = rng.uniform(0, sh2)
        draw.line([(sx2 + x, sy2 + y), (sx2 + x + rng.uniform(-14, 14),
                                        sy2 + y + rng.uniform(8, 20))],
                  fill=(86, 76, 48), width=1)

    # --- leather: grain, stitching, belt holes ----------------------------
    lx, ly, lw, lh = rect("leather")
    for _ in range(700):
        x = rng.uniform(0, lw)
        y = rng.uniform(0, lh)
        r = rng.uniform(0.5, 2.5)
        draw.ellipse([lx + x - r, ly + y - r, lx + x + r, ly + y + r],
                     fill=(38, 23, 13))
    for i in range(0, lh, 26):
        draw.line([(lx + 8, ly + i), (lx + 20, ly + i)], fill=(72, 52, 32), width=2)
    for i in range(0, lw, 28):
        draw.ellipse([lx + i, ly + lh - 16, lx + i + 6, ly + lh - 10],
                     fill=(18, 12, 8))

    # --- canvas pack: flap, stitching, straps -----------------------------
    cx, cy, cw, ch = rect("canvas")
    draw.rectangle([cx + 20, cy + 20, cx + cw - 20, cy + 150],
                   fill=(126, 112, 78), outline=(78, 68, 42), width=4)
    for i in range(0, cw, 24):
        draw.line([(cx + i, cy + 12), (cx + i, cy + 170)], fill=(104, 92, 62), width=1)
    for _ in range(40):
        x = rng.uniform(0, cw)
        y = rng.uniform(0, ch)
        draw.ellipse([cx + x - 3, cy + y - 3, cx + x + 3, cy + y + 3],
                     fill=(60, 52, 34))
    # Metal rivets.
    for rx in (40, cw - 48):
        for ry in (32, 140):
            draw.ellipse([cx + rx, cy + ry, cx + rx + 14, cy + ry + 14],
                         fill=(120, 122, 118), outline=(48, 48, 46), width=2)

    # --- boot: laces and eyelets ------------------------------------------
    bx, by, bw, bh = rect("boot")
    draw.rectangle([bx + 60, by + 20, bx + 196, by + 130],
                   fill=(58, 41, 28), outline=(32, 22, 15), width=3)
    for i in range(4):
        yy = by + 40 + i * 24
        draw.line([(bx + 62, yy), (bx + 194, yy + 9)], fill=(26, 18, 12), width=3)
        draw.line([(bx + 64, yy + 10), (bx + 192, yy)], fill=(96, 82, 62), width=1)
        draw.ellipse([bx + 54, yy - 5, bx + 68, yy + 9], fill=(54, 54, 52))
        draw.ellipse([bx + 188, yy - 5, bx + 202, yy + 9], fill=(54, 54, 52))
    for _ in range(120):
        x = rng.uniform(0, bw)
        y = rng.uniform(0, bh)
        draw.point((bx + x, by + y), fill=(52, 34, 20))

    # --- sole tread -------------------------------------------------------
    ox, oy, ow, oh = rect("sole")
    for i in range(0, oh, 16):
        draw.line([(ox, oy + i), (ox + ow, oy + i)], fill=(24, 24, 22), width=4)
    for i in range(0, ow, 26):
        draw.line([(ox + i, oy), (ox + i, oy + oh)], fill=(20, 20, 19), width=2)

    # --- bedroll with tie bands ------------------------------------------
    rx, ry, rw, rh = rect("bedroll")
    draw.rectangle([rx + 70, ry, rx + 104, ry + rh], fill=(54, 58, 32))
    draw.rectangle([rx + rw - 104, ry, rx + rw - 70, ry + rh], fill=(54, 58, 32))
    for _ in range(90):
        x = rng.uniform(0, rw)
        y = rng.uniform(0, rh)
        draw.line([(rx + x, ry + y), (rx + x + 10, ry + y + 2)],
                  fill=(62, 66, 38), width=1)

    # --- metal brushed highlights ----------------------------------------
    mx, my, mw, mh = rect("metal")
    for _ in range(500):
        x = rng.uniform(0, mw)
        y = rng.uniform(0, mh)
        draw.line([(mx + x, my + y), (mx + x + rng.uniform(2, 20), my + y)],
                  fill=(150, 152, 150), width=1)

    # --- hair strands -----------------------------------------------------
    hx, hy, hw, hh = rect("hair")
    for _ in range(900):
        x = rng.uniform(0, hw)
        y = rng.uniform(0, hh)
        draw.line([(hx + x, hy + y), (hx + x + rng.uniform(-4, 4),
                                      hy + y + rng.uniform(10, 34))],
                  fill=(44, 28, 18), width=1)

    image.save(path)
    return path


# ---------------------------------------------------------------------------
# mesh builder
# ---------------------------------------------------------------------------


class Builder:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []

    def vertex(self, position, uv, normal):
        self.positions.append(tuple(float(v) for v in position))
        self.normals.append(tuple(float(v) for v in normal))
        self.uvs.append(tuple(float(v) for v in uv))
        return len(self.positions) - 1

    def triangle(self, a, b, c):
        self.indices.extend((a, b, c))

    def quad(self, points, uvs, normal=None):
        p0, p1, p2, p3 = points
        u0, u1, u2, u3 = uvs
        if normal is None:
            e1 = np.asarray(p1) - np.asarray(p0)
            e2 = np.asarray(p2) - np.asarray(p0)
            n = np.cross(e1, e2)
            length = np.linalg.norm(n)
            normal = n / length if length > 1e-9 else np.array([0.0, 1.0, 0.0])
        a = self.vertex(p0, u0, normal)
        b = self.vertex(p1, u1, normal)
        c = self.vertex(p2, u2, normal)
        d = self.vertex(p3, u3, normal)
        self.triangle(a, b, c)
        self.triangle(a, c, d)

    def loft(self, centers, radii, segments, rect, cap_start=True, cap_end=True,
             flip_v=False):
        centers = [np.asarray(c, dtype=np.float64) for c in centers]
        count = len(centers)
        if isinstance(radii[0], (int, float)):
            radii = [(float(r), float(r)) for r in radii]
        base = len(self.positions)
        for i, center in enumerate(centers):
            if i == 0:
                tangent = centers[1] - centers[0]
            elif i == count - 1:
                tangent = centers[-1] - centers[-2]
            else:
                tangent = centers[i + 1] - centers[i - 1]
            tangent /= max(np.linalg.norm(tangent), 1e-9)
            ref = np.array([0.0, 1.0, 0.0])
            if abs(float(np.dot(tangent, ref))) > 0.95:
                ref = np.array([0.0, 0.0, 1.0])
            side = np.cross(ref, tangent)
            side /= max(np.linalg.norm(side), 1e-9)
            forward = np.cross(tangent, side)
            forward /= max(np.linalg.norm(forward), 1e-9)
            rx, rz = radii[i]
            v = (i / (count - 1)) if count > 1 else 0.0
            if flip_v:
                v = 1.0 - v
            for j in range(segments + 1):
                theta = -PI + TAU * (j / segments)
                sin_t = math.sin(theta)
                cos_t = math.cos(theta)
                point = center + side * (sin_t * rx) + forward * (cos_t * rz)
                normal = side * (sin_t / max(rx, 1e-6)) + forward * (cos_t / max(rz, 1e-6))
                normal /= max(np.linalg.norm(normal), 1e-9)
                u = 0.5 + theta / TAU
                self.vertex(point, rect_uv(rect, u, v), normal)
        stride = segments + 1
        for i in range(count - 1):
            for j in range(segments):
                a = base + i * stride + j
                b = base + (i + 1) * stride + j
                c = a + 1
                d = b + 1
                self.triangle(a, b, c)
                self.triangle(c, b, d)
        if cap_start:
            center = centers[0]
            tangent = centers[1] - centers[0]
            tangent /= max(np.linalg.norm(tangent), 1e-9)
            normal = -tangent
            v = 1.0 if flip_v else 0.0
            ci = self.vertex(center, rect_uv(rect, 0.5, v), normal)
            for j in range(segments):
                a = base + j
                b = base + j + 1
                self.triangle(ci, b, a)
        if cap_end:
            center = centers[-1]
            tangent = centers[-1] - centers[-2]
            tangent /= max(np.linalg.norm(tangent), 1e-9)
            normal = tangent
            v = 0.0 if flip_v else 1.0
            ci = self.vertex(center, rect_uv(rect, 0.5, v), normal)
            last = base + (count - 1) * stride
            for j in range(segments):
                a = last + j
                b = last + j + 1
                self.triangle(ci, a, b)

    def ellipsoid(self, center, radii, segments, rings, rect, flip_v=False,
                  v_from=0.0, v_to=1.0):
        center = np.asarray(center, dtype=np.float64)
        rx, ry, rz = radii
        base = len(self.positions)
        for ring in range(rings + 1):
            phi = PI * ring / rings
            v = v_from + (v_to - v_from) * (ring / rings)
            if flip_v:
                v = 1.0 - v
            sin_phi = math.sin(phi)
            cos_phi = math.cos(phi)
            for seg in range(segments + 1):
                theta = -PI + TAU * seg / segments
                sin_t = math.sin(theta)
                cos_t = math.cos(theta)
                point = center + np.array(
                    [rx * sin_phi * sin_t, ry * cos_phi, rz * sin_phi * cos_t]
                )
                normal = np.array(
                    [sin_phi * sin_t / max(rx, 1e-6),
                     cos_phi / max(ry, 1e-6),
                     sin_phi * cos_t / max(rz, 1e-6)]
                )
                normal /= max(np.linalg.norm(normal), 1e-9)
                u = 0.5 + theta / TAU
                self.vertex(point, rect_uv(rect, u, v), normal)
        stride = segments + 1
        for ring in range(rings):
            for seg in range(segments):
                a = base + ring * stride + seg
                b = a + stride
                c = a + 1
                d = b + 1
                self.triangle(a, b, c)
                self.triangle(c, b, d)

    def head(self, center, radii, segments=40, rings=28):
        center = np.asarray(center, dtype=np.float64)
        rx, ry, rz = radii
        base = len(self.positions)
        for ring in range(rings + 1):
            phi = PI * ring / rings
            cos_phi = math.cos(phi)
            sin_phi = math.sin(phi)
            lower = max(0.0, -cos_phi)
            width_factor = 1.0 - 0.20 * lower
            depth_factor = 1.0 - 0.10 * lower
            chin = 0.012 * lower
            for seg in range(segments + 1):
                theta = -PI + TAU * seg / segments
                sin_t = math.sin(theta)
                cos_t = math.cos(theta)
                point = center + np.array(
                    [rx * sin_phi * sin_t * width_factor,
                     ry * cos_phi,
                     rz * sin_phi * cos_t * depth_factor + chin]
                )
                normal = np.array(
                    [sin_phi * sin_t / max(rx, 1e-6),
                     cos_phi / max(ry, 1e-6),
                     sin_phi * cos_t / max(rz, 1e-6)]
                )
                normal /= max(np.linalg.norm(normal), 1e-9)
                u = 0.5 + theta / TAU
                v = ring / rings
                self.vertex(point, rect_uv("face", u, v), normal)
        stride = segments + 1
        for ring in range(rings):
            for seg in range(segments):
                a = base + ring * stride + seg
                b = a + stride
                c = a + 1
                d = b + 1
                self.triangle(a, b, c)
                self.triangle(c, b, d)

    def box(self, center, half, rect, rotation_y=0.0):
        center = np.asarray(center, dtype=np.float64)
        hx, hy, hz = half
        ca = math.cos(rotation_y)
        sa = math.sin(rotation_y)

        def rot(point):
            x, y, z = point
            return np.array([x * ca + z * sa, y, -x * sa + z * ca])

        corners = [
            (-hx, -hy, -hz), (hx, -hy, -hz), (hx, hy, -hz), (-hx, hy, -hz),
            (-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz),
        ]
        corners = [center + rot(c) for c in corners]
        faces = [
            ((4, 5, 6, 7), (0.0, 1.0)),  # +Z
            ((1, 0, 3, 2), (0.0, 1.0)),  # -Z
            ((5, 1, 2, 6), (0.0, 1.0)),  # +X
            ((0, 4, 7, 3), (0.0, 1.0)),  # -X
            ((7, 6, 2, 3), (0.0, 1.0)),  # +Y
            ((0, 1, 5, 4), (0.0, 1.0)),  # -Y
        ]
        for indices, _ in faces:
            p = [corners[i] for i in indices]
            e1 = p[1] - p[0]
            e2 = p[2] - p[0]
            n = np.cross(e1, e2)
            n /= max(np.linalg.norm(n), 1e-9)
            self.quad(
                p,
                [rect_uv(rect, 0, 0), rect_uv(rect, 1, 0),
                 rect_uv(rect, 1, 1), rect_uv(rect, 0, 1)],
                n,
            )

    def band(self, center, radius_x, radius_z, height, segments, rect):
        centers = [
            np.array([center[0], center[1] - height * 0.5, center[2]]),
            np.array([center[0], center[1] + height * 0.5, center[2]]),
        ]
        self.loft(centers, [(radius_x, radius_z), (radius_x, radius_z)],
                  segments, rect, cap_start=False, cap_end=False)

    def disc(self, center, radius_x, radius_z, thickness, segments, rect, rect_side=None):
        top = np.array([center[0], center[1] + thickness * 0.5, center[2]])
        bottom = np.array([center[0], center[1] - thickness * 0.5, center[2]])
        self.loft([bottom, top], [(radius_x, radius_z), (radius_x, radius_z)],
                  segments, rect_side or rect, cap_start=True, cap_end=True)


def build_avatar(path):
    b = Builder()

    # --- pelvis and torso -------------------------------------------------
    b.loft(
        [(0, 0.86, 0.0), (0, 0.95, 0.0), (0, 1.02, 0.0)],
        [(0.18, 0.125), (0.195, 0.135), (0.195, 0.135)],
        26, "pants", cap_start=True, cap_end=False, flip_v=True,
    )
    b.loft(
        [(0, 0.99, 0.0), (0, 1.10, 0.0), (0, 1.24, 0.006),
         (0, 1.37, 0.006), (0, 1.46, 0.0)],
        [(0.19, 0.13), (0.20, 0.14), (0.217, 0.150),
         (0.226, 0.145), (0.215, 0.128)],
        28, "shirt", cap_start=True, cap_end=False, flip_v=True,
    )
    # Hem over the belt.
    b.loft(
        [(0, 0.985, 0.0), (0, 1.01, 0.0)],
        [(0.205, 0.142), (0.205, 0.142)],
        28, "shirt_plain", cap_start=False, cap_end=False,
    )
    # Collar.
    b.loft(
        [(0, 1.48, 0.0), (0, 1.545, 0.0)],
        [(0.095, 0.085), (0.115, 0.10)],
        24, "shirt_plain", cap_start=False, cap_end=False,
    )
    # Neck.
    b.loft(
        [(0, 1.47, 0.0), (0, 1.60, 0.005)],
        [(0.058, 0.055), (0.062, 0.058)],
        20, "skin", cap_start=False, cap_end=False,
    )
    # Belt and buckle.
    b.band((0, 1.025, 0.0), 0.207, 0.143, 0.055, 28, "leather")
    b.box((0, 1.025, 0.150), (0.048, 0.035, 0.015), "metal")
    # Chest buttons.
    for y in (1.10, 1.22, 1.34):
        b.ellipsoid((0, y, 0.157), (0.012, 0.012, 0.008), 12, 8, "metal")
    # Chest pockets live in the shirt texture; no bulky boxes.
    # Backpack straps over the chest.
    for x in (-0.10, 0.10):
        b.loft(
            [(x, 1.45, 0.115), (x, 1.25, 0.14), (x, 1.10, 0.145)],
            [(0.012, 0.007), (0.012, 0.007), (0.012, 0.007)],
            10, "leather", cap_start=False, cap_end=False,
        )
        b.box((x, 1.34, 0.152), (0.016, 0.012, 0.008), "metal")
    # Scarf.
    b.band((0, 1.53, 0.005), 0.092, 0.082, 0.045, 24, "scarf")
    b.loft(
        [(-0.045, 1.50, 0.09), (-0.055, 1.36, 0.11), (-0.060, 1.24, 0.10)],
        [(0.026, 0.008), (0.032, 0.008), (0.024, 0.006)],
        10, "scarf", cap_start=True, cap_end=True,
    )

    # --- legs -------------------------------------------------------------
    leg_specs = [
        ((0.105, 0.94, 0.02), (0.135, 0.50, 0.11), (0.125, 0.13, 0.17)),
        ((-0.105, 0.94, -0.02), (-0.125, 0.50, -0.07), (-0.125, 0.13, -0.10)),
    ]
    for hip, knee, ankle in leg_specs:
        hip = np.asarray(hip, dtype=np.float64)
        knee = np.asarray(knee, dtype=np.float64)
        ankle = np.asarray(ankle, dtype=np.float64)
        mid_thigh = hip * 0.55 + knee * 0.45
        mid_shin = knee * 0.55 + ankle * 0.45
        b.loft([hip, mid_thigh, knee], [(0.094, 0.094), (0.087, 0.087), (0.070, 0.071)],
               18, "pants_plain", cap_start=True, cap_end=False)
        b.ellipsoid(knee, (0.070, 0.068, 0.070), 18, 12, "pants_plain")
        b.loft([knee, mid_shin, ankle], [(0.071, 0.070), (0.061, 0.061), (0.051, 0.053)],
               16, "pants_plain", cap_start=False, cap_end=False)
        # Boot shaft.
        shaft_top = ankle + np.array([0.0, 0.16, -0.02])
        b.loft([ankle, ankle + np.array([0.0, 0.08, -0.01]), shaft_top],
               [(0.060, 0.058), (0.064, 0.062), (0.066, 0.064)],
               16, "boot", cap_start=False, cap_end=True)
        # Foot body and toe.
        foot_center = ankle + np.array([0.0, -0.075, 0.075])
        b.ellipsoid(foot_center, (0.070, 0.055, 0.115), 20, 12, "boot")
        b.ellipsoid(ankle + np.array([0.0, -0.085, 0.155]), (0.062, 0.045, 0.075),
                    18, 10, "boot")
        # Sole.
        b.box((ankle[0], 0.022, ankle[2] + 0.06), (0.073, 0.022, 0.150), "sole")
        b.box((ankle[0], 0.030, ankle[2] - 0.055), (0.068, 0.020, 0.055), "sole")
        # Laces.
        for i in range(4):
            y = ankle[1] - 0.005 + i * 0.026
            z = ankle[2] + 0.105 - i * 0.010
            b.box((ankle[0], y, z), (0.036, 0.005, 0.006), "boot", rotation_y=0.12)
        # Eyelets.
        for x in (-0.035, 0.035):
            for i in range(4):
                b.ellipsoid((ankle[0] + x, ankle[1] - 0.005 + i * 0.026,
                             ankle[2] + 0.098 - i * 0.010),
                            (0.005, 0.005, 0.005), 8, 6, "metal")
        # Cargo pocket on the outer thigh.
        side = 1.0 if hip[0] > 0 else -1.0
        b.box((side * 0.175, 0.67, 0.02), (0.012, 0.075, 0.065), "pants_plain")
        b.box((side * 0.180, 0.735, 0.02), (0.014, 0.018, 0.070), "leather")

    # --- arms -------------------------------------------------------------
    arm_specs = [
        (np.array([0.245, 1.455, 0.0]), np.array([0.305, 1.22, 0.075]),
         np.array([0.295, 1.035, 0.24])),
        (np.array([-0.245, 1.455, 0.0]), np.array([-0.315, 1.22, -0.035]),
         np.array([-0.305, 1.035, 0.055])),
    ]
    for shoulder, elbow, wrist in arm_specs:
        b.ellipsoid(shoulder, (0.063, 0.070, 0.063), 18, 12, "shirt_plain")
        mid_upper = shoulder * 0.5 + elbow * 0.5
        b.loft([shoulder, mid_upper, elbow],
               [(0.072, 0.072), (0.066, 0.066), (0.058, 0.058)],
               16, "shirt_plain", cap_start=False, cap_end=False)
        # Rolled cuff.
        cuff = elbow + (elbow - shoulder) * 0.02
        b.band(cuff, 0.058, 0.058, 0.045, 14, "shirt_plain")
        mid_fore = elbow * 0.5 + wrist * 0.5
        b.loft([elbow, mid_fore, wrist],
               [(0.054, 0.054), (0.050, 0.050), (0.041, 0.041)],
               14, "skin", cap_start=False, cap_end=False)
        # Hand.
        direction = wrist - elbow
        direction /= max(np.linalg.norm(direction), 1e-9)
        palm = wrist + direction * 0.045
        b.ellipsoid(palm, (0.038, 0.052, 0.030), 16, 10, "skin")
        # Four fingers and a thumb.
        up = np.array([0.0, 1.0, 0.0])
        side = np.cross(direction, up)
        side /= max(np.linalg.norm(side), 1e-9)
        up2 = np.cross(side, direction)
        up2 /= max(np.linalg.norm(up2), 1e-9)
        for fi in range(4):
            offset = (fi - 1.5) * 0.018
            start = palm + direction * 0.025 + side * offset + up2 * 0.004
            end = start + direction * (0.060 - abs(fi - 1.5) * 0.005) + up2 * 0.005
            b.loft([start, (start + end) * 0.5, end],
                   [(0.009, 0.009), (0.008, 0.008), (0.006, 0.006)],
                   8, "skin", cap_start=False, cap_end=True)
        thumb_start = palm + side * (-0.030 if shoulder[0] > 0 else 0.030) - direction * 0.005
        thumb_end = thumb_start + side * (-0.035 if shoulder[0] > 0 else 0.035) + direction * 0.030
        b.loft([thumb_start, (thumb_start + thumb_end) * 0.5, thumb_end],
               [(0.011, 0.011), (0.009, 0.009), (0.006, 0.006)],
               8, "skin", cap_start=False, cap_end=True)

    # --- head -------------------------------------------------------------
    head_center = (0.0, 1.655, 0.015)
    b.head(head_center, (0.098, 0.128, 0.108), 40, 28)
    # Nose and ears.
    b.ellipsoid((0.0, 1.635, 0.118), (0.024, 0.040, 0.045), 16, 10, "skin")
    for x in (-0.096, 0.096):
        b.ellipsoid((x, 1.655, 0.005), (0.017, 0.035, 0.026), 14, 8, "skin")
    # Hair under the hat: a back cap and sideburns, dark enough to separate
    # the head from the canvas brim.
    b.ellipsoid((0.0, 1.685, -0.030), (0.094, 0.108, 0.072), 26, 16, "hair")
    for x in (-0.092, 0.092):
        b.ellipsoid((x, 1.625, 0.025), (0.022, 0.060, 0.022), 12, 8, "hair")
    # Hat brim.
    b.disc((0.0, 1.727, 0.015), 0.215, 0.215, 0.014, 40, "canvas")
    # Crown.
    b.loft(
        [(0.0, 1.727, 0.015), (0.0, 1.782, 0.015), (0.0, 1.842, 0.015)],
        [(0.125, 0.120), (0.122, 0.117), (0.104, 0.100)],
        32, "canvas", cap_start=False, cap_end=True,
    )
    # Hat band.
    b.band((0.0, 1.764, 0.015), 0.127, 0.122, 0.035, 32, "leather")
    # Neck scarf is already at 1.53; the collar hides the seam.

    # --- backpack ---------------------------------------------------------
    b.ellipsoid((0.0, 1.300, -0.235), (0.135, 0.175, 0.082), 26, 18, "canvas_plain")
    # Flap.
    b.box((0.0, 1.450, -0.235), (0.132, 0.070, 0.100), "canvas", rotation_y=0.0)
    b.box((0.0, 1.492, -0.225), (0.137, 0.020, 0.104), "leather")
    # Side pouches.
    for x in (-0.165, 0.165):
        b.box((x, 1.285, -0.235), (0.040, 0.070, 0.055), "canvas_plain", rotation_y=0.0)
        b.box((x, 1.355, -0.245), (0.046, 0.016, 0.064), "leather")
    # Straps and buckles.
    for x in (-0.10, 0.10):
        b.loft(
            [(x, 1.47, -0.17), (x * 1.1, 1.34, -0.20), (x * 1.2, 1.18, -0.17)],
            [(0.020, 0.012), (0.020, 0.012), (0.020, 0.012)],
            10, "leather", cap_start=False, cap_end=False,
        )
        b.box((x * 1.2, 1.22, -0.165), (0.024, 0.018, 0.012), "metal")
    # Bedroll.
    b.loft(
        [(-0.22, 1.535, -0.245), (0.22, 1.535, -0.245)],
        [(0.070, 0.070), (0.070, 0.070)],
        18, "bedroll", cap_start=True, cap_end=True,
    )
    for x in (-0.14, 0.14):
        b.band((x, 1.535, -0.245), 0.074, 0.074, 0.030, 16, "leather")
    # Canteen at the left hip.
    b.loft(
        [(-0.235, 0.985, -0.035), (-0.255, 0.985, -0.035)],
        [(0.062, 0.062), (0.062, 0.062)],
        18, "metal", cap_start=True, cap_end=True,
    )
    b.loft(
        [(-0.270, 1.005, -0.035), (-0.285, 1.030, -0.035)],
        [(0.018, 0.018), (0.020, 0.020)],
        10, "leather", cap_start=True, cap_end=True,
    )
    # Machete sheath on the right thigh.
    b.loft(
        [(0.215, 0.94, 0.03), (0.235, 0.70, 0.055), (0.245, 0.47, 0.06)],
        [(0.028, 0.016), (0.030, 0.018), (0.020, 0.012)],
        10, "leather", cap_start=True, cap_end=True,
    )
    b.loft(
        [(0.245, 0.47, 0.06), (0.248, 0.42, 0.06)],
        [(0.022, 0.014), (0.018, 0.010)],
        8, "metal", cap_start=True, cap_end=True,
    )
    b.loft(
        [(0.208, 0.94, 0.03), (0.205, 0.99, 0.03)],
        [(0.018, 0.018), (0.018, 0.018)],
        10, "leather", cap_start=True, cap_end=True,
    )
    b.box((0.206, 1.005, 0.03), (0.032, 0.022, 0.018), "leather", rotation_y=0.0)
    # Belt pouch.
    b.box((0.195, 1.00, 0.07), (0.045, 0.055, 0.040), "canvas")
    b.box((0.195, 1.045, 0.07), (0.048, 0.014, 0.043), "leather")

    return b


# ---------------------------------------------------------------------------
# GLB writer
# ---------------------------------------------------------------------------


def write_glb(path, positions, normals, uvs, indices):
    positions = np.asarray(positions, dtype=np.float32)
    normals = np.asarray(normals, dtype=np.float32)
    uvs = np.asarray(uvs, dtype=np.float32)
    indices = np.asarray(indices, dtype=np.uint32)

    def pad4(data):
        while len(data) % 4:
            data += b"\x00"
        return data

    chunks = [positions.tobytes(), normals.tobytes(), uvs.tobytes(), indices.tobytes()]
    offsets = []
    blob = b""
    for chunk in chunks:
        offsets.append(len(blob))
        blob += pad4(chunk)
    views = [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(chunks[0]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(chunks[1]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(chunks[2]), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(chunks[3]), "target": 34963},
    ]
    accessors = [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": int(positions.shape[0]),
            "type": "VEC3",
            "min": [float(v) for v in positions.min(axis=0)],
            "max": [float(v) for v in positions.max(axis=0)],
        },
        {"bufferView": 1, "componentType": 5126, "count": int(normals.shape[0]), "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": int(uvs.shape[0]), "type": "VEC2"},
        {"bufferView": 3, "componentType": 5125, "count": int(indices.shape[0]), "type": "SCALAR"},
    ]
    gltf = {
        "asset": {"version": "2.0", "generator": "lotus jungle_avatar.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Explorer"}],
        "meshes": [
            {
                "name": "Explorer",
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                        "indices": 3,
                    }
                ],
            }
        ],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    json_bytes = pad4(json.dumps(gltf, separators=(",", ":")).encode())
    bin_bytes = pad4(blob)
    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    out = b"glTF" + struct.pack("<II", 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    out += struct.pack("<II", len(bin_bytes), 0x004E4942) + bin_bytes
    with open(path, "wb") as handle:
        handle.write(out)


def build_avatar_assets(texture_path, mesh_path, face_source=None):
    build_atlas(texture_path, face_source)
    builder = build_avatar(mesh_path)
    write_glb(mesh_path, builder.positions, builder.normals, builder.uvs,
              builder.indices)
    return len(builder.positions), len(builder.indices) // 3

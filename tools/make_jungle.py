#!/usr/bin/env python3
"""Builds scenes/jungle.json and the assets it needs.

Run from the repo root:  python3 tools/make_jungle.py

Outputs:
  assets/jungle_terrain.glb           heightfield floor (HeightFieldCollider)
  assets/jungle_path.glb              dirt trail ribbon
  assets/jungle_stream.glb            water ribbon
  assets/textures/jungle_*.png        seamless albedo textures
  assets/animations/jungle_idle.json  breathing idle for the explorer
  assets/animations/jungle_sway.json  canopy sway
  scenes/jungle.json                  the scene itself

Everything is deterministic: the same script produces the same bytes.  The
scene is authored directly as version-3 JSON because it has ~1500 entities;
loading it through the console would be slower and would lose the nesting.
The engine still validates it on load, and `--dump-state` round-trips it.

Layout: the player starts on the trail at z=+8 facing -Z; the trail winds
north, crosses a stream at z~-12 over a plank bridge, and disappears into the
fog.  Hero trees frame the shot at x=-3.4/z=6 and x=+3.6/z=4.
"""

import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jungle_avatar import build_avatar_assets  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENE_PATH = os.path.join(ROOT, "scenes", "jungle.json")
ASSETS_PATH = os.path.join(ROOT, "assets", "assets.json")
TEXTURE_DIR = os.path.join(ROOT, "assets", "textures")
ANIMATION_DIR = os.path.join(ROOT, "assets", "animations")

PI = math.pi
TAU = 2.0 * math.pi
SEED = 7


# --------------------------------------------------------------------------
# deterministic math helpers
# --------------------------------------------------------------------------


def smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def clamp(x, lo, hi):
    return max(lo, min(hi, x))


def lerp(a, b, t):
    return a + (b - a) * t


def stream_center(x):
    """Z of the stream centreline at world X."""
    return -12.6 + 2.1 * math.sin(x * 0.075 + 0.5) + 0.65 * math.sin(x * 0.21)


def path_center(z):
    """X of the trail centreline at world Z."""
    return 2.0 * math.sin((z - 4.0) * 0.11) + 0.5 * math.sin((z + 6.0) * 0.23)


WATER_LEVEL = -0.05


def terrain_height(x, z):
    """Smooth valley floor with a carved stream channel and rising rim."""
    h = 0.55
    h += 0.34 * math.sin(x * 0.085 + 0.6) * math.sin(z * 0.075 + 1.1)
    h += 0.14 * math.sin(x * 0.19 - 0.4) * math.sin(z * 0.16 + 0.2)
    h += 0.05 * math.sin(x * 0.47 + 2.0) * math.sin(z * 0.41 - 1.0)

    # The valley wall keeps the horizon covered past the fog line.
    rim = smoothstep(31.0, 57.0, math.hypot(x, z + 6.0))
    h += rim * 4.5

    # River channel: flat bed, soft banks.
    d = abs(z - stream_center(x))
    t = 1.0 - smoothstep(1.15, 3.9, d)
    h = h * (1.0 - t) + (-0.42) * t
    return h


# --------------------------------------------------------------------------
# seamless texture noise
# --------------------------------------------------------------------------


def pnoise2(size, fx, fy, rng):
    """Periodic smooth value noise sampled at `size` x `size`."""
    grid = rng.random((fy, fx)).astype(np.float32)
    xs = np.arange(size, dtype=np.float32) * (fx / size)
    ys = np.arange(size, dtype=np.float32) * (fy / size)
    ix = np.floor(xs).astype(np.int32)
    iy = np.floor(ys).astype(np.int32)
    tx = xs - ix
    ty = ys - iy
    tx = tx * tx * (3.0 - 2.0 * tx)
    ty = ty * ty * (3.0 - 2.0 * ty)
    ix0 = ix % fx
    iy0 = iy % fy
    ix1 = (ix0 + 1) % fx
    iy1 = (iy0 + 1) % fy
    a = grid[np.ix_(iy0, ix0)]
    b = grid[np.ix_(iy0, ix1)]
    c = grid[np.ix_(iy1, ix0)]
    d = grid[np.ix_(iy1, ix1)]
    tx = tx[None, :]
    ty = ty[:, None]
    return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (
        c * (1.0 - tx) + d * tx
    ) * ty


def fbm2(size, fx, fy, octaves, rng, gain=0.5):
    value = np.zeros((size, size), dtype=np.float32)
    amplitude = 1.0
    total = 0.0
    for octave in range(octaves):
        value += pnoise2(size, fx * (2**octave), fy * (2**octave), rng) * amplitude
        total += amplitude
        amplitude *= gain
    return value / total


def pnoise1(size, freq, rng):
    grid = rng.random(freq).astype(np.float32)
    xs = np.arange(size, dtype=np.float32) * (freq / size)
    ix = np.floor(xs).astype(np.int32)
    tx = xs - ix
    tx = tx * tx * (3.0 - 2.0 * tx)
    i0 = ix % freq
    i1 = (i0 + 1) % freq
    return grid[i0] * (1.0 - tx) + grid[i1] * tx


def mix_color(a, b, t):
    t = t[..., None] if isinstance(t, np.ndarray) else t
    return np.asarray(a, dtype=np.float32) * (1.0 - t) + np.asarray(
        b, dtype=np.float32
    ) * t


def to_image(rgb, supersample=1):
    data = np.clip(rgb * 255.0, 0, 255).astype(np.uint8)
    image = Image.fromarray(data, "RGB")
    if supersample > 1:
        image = image.resize(
            (image.width // supersample, image.height // supersample),
            Image.LANCZOS,
        )
    return image


def save_png(name, image):
    path = os.path.join(TEXTURE_DIR, name)
    image.save(path)
    print(f"  texture {name} {image.width}x{image.height}")


def leaf_points(cx, cy, length, width, angle, steps=10):
    points = []
    for i in range(steps):
        t = TAU * i / steps
        x = 0.5 * length * math.cos(t)
        y = 0.5 * width * math.sin(t) * (0.65 + 0.35 * abs(math.cos(t)))
        xr = x * math.cos(angle) - y * math.sin(angle)
        yr = x * math.sin(angle) + y * math.cos(angle)
        points.append((cx + xr, cy + yr))
    return points


def wrapped_draw(draw, canvas, fn):
    """Calls fn(dx, dy) for the 3x3 copies so edge shapes wrap seamlessly."""
    size = canvas
    fn(0.0, 0.0)
    fn(size, 0.0)
    fn(-size, 0.0)
    fn(0.0, size)
    fn(0.0, -size)
    fn(size, size)
    fn(-size, size)
    fn(size, -size)
    fn(-size, -size)


# --------------------------------------------------------------------------
# textures
# --------------------------------------------------------------------------


def texture_leaf_grain(name, bright):
    """Mottled leaf surface with a midrib; mapped onto pointed leaf geometry."""
    rng = np.random.default_rng(SEED + (20 if bright else 21))
    size = 512
    n = fbm2(size, 7, 4, 5, rng)
    if bright:
        dark = (0.10, 0.20, 0.06)
        mid = (0.24, 0.40, 0.13)
        vein = (0.36, 0.50, 0.20)
        spots = (0.42, 0.54, 0.24)
    else:
        dark = (0.045, 0.11, 0.030)
        mid = (0.14, 0.27, 0.075)
        vein = (0.24, 0.36, 0.13)
        spots = (0.30, 0.42, 0.16)
    rgb = mix_color(dark, mid, n)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    # Central midrib (u ~ 0.5) and angled side veins.
    rib = np.exp(-(((xx / size) - 0.5) / 0.018) ** 2)
    side = (
        np.sin((yy / size) * 46.0 - np.abs(xx / size - 0.5) * 34.0)
        > 0.88
    ).astype(np.float32)
    rgb = mix_color(rgb, vein, rib * 0.55 + side * 0.12)
    blemish = fbm2(size, 18, 18, 3, rng)
    rgb = mix_color(rgb, spots, np.clip((blemish - 0.60) / 0.22, 0.0, 1.0) * 0.6)
    arr = np.clip(rgb * 255.0 + rng.normal(0.0, 2.0, (size, size, 3)), 0, 255)
    save_png(name, to_image(arr / 255.0))


def texture_ground():
    rng = np.random.default_rng(SEED + 1)
    size = 1024
    detail = size * 2
    soil_dark = (0.07, 0.055, 0.035)
    soil_mid = (0.22, 0.18, 0.10)
    moss = (0.11, 0.20, 0.055)

    n = fbm2(size, 4, 4, 5, rng)
    rgb = mix_color(soil_dark, soil_mid, n)
    moss_n = fbm2(size, 7, 7, 4, rng)
    mask = np.clip((moss_n - 0.43) / 0.28, 0.0, 1.0) * 0.75
    rgb = mix_color(rgb, moss, mask)

    image = to_image(rgb).resize((detail, detail), Image.LANCZOS)
    draw = ImageDraw.Draw(image)
    leaf_colors = [
        (52, 42, 25),
        (66, 50, 28),
        (80, 60, 32),
        (44, 52, 25),
        (60, 56, 29),
        (96, 70, 36),
        (36, 31, 19),
        (70, 44, 24),
        (50, 62, 28),
    ]
    for _ in range(760):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        length = rng.uniform(22, 82)
        width = length * rng.uniform(0.30, 0.55)
        angle = rng.uniform(0, TAU)
        color = leaf_colors[int(rng.integers(0, len(leaf_colors)))]
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.polygon(
                leaf_points(x + dx, y + dy, length, width, angle), fill=color
            ),
        )
    for _ in range(70):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        angle = rng.uniform(0, TAU)
        length = rng.uniform(20, 80)
        x2 = x + math.cos(angle) * length
        y2 = y + math.sin(angle) * length
        color = (30, 22, 14)
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.line(
                [(x + dx, y + dy), (x2 + dx, y2 + dy)],
                fill=color,
                width=int(rng.integers(1, 3)),
            ),
        )
    for _ in range(90):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        r = rng.uniform(2, 6)
        shade = int(rng.uniform(48, 88))
        color = (shade, shade - 4, shade - 8)
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.ellipse(
                (x + dx - r, y + dy - r, x + dx + r, y + dy + r), fill=color
            ),
        )
    image = image.resize((size, size), Image.LANCZOS)
    arr = np.asarray(image).astype(np.float32)
    arr += rng.normal(0.0, 2.4, arr.shape)
    save_png("jungle_ground.png", to_image(arr / 255.0))


def texture_path():
    rng = np.random.default_rng(SEED + 2)
    size = 512
    detail = size * 2
    n = fbm2(size, 5, 5, 5, rng)
    rgb = mix_color((0.13, 0.095, 0.058), (0.30, 0.23, 0.14), n)
    image = to_image(rgb).resize((detail, detail), Image.LANCZOS)
    draw = ImageDraw.Draw(image)
    for _ in range(300):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        length = rng.uniform(6, 22)
        width = length * rng.uniform(0.3, 0.6)
        angle = rng.uniform(0, TAU)
        shade = int(rng.uniform(30, 70))
        color = (shade + 10, shade, shade - 10)
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.polygon(
                leaf_points(x + dx, y + dy, length, width, angle), fill=color
            ),
        )
    for _ in range(45):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        r = rng.uniform(3, 11)
        shade = int(rng.uniform(48, 88))
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.ellipse(
                (x + dx - r, y + dy - r / 2, x + dx + r, y + dy + r / 2),
                fill=(shade, shade - 5, shade - 12),
            ),
        )
    image = image.resize((size, size), Image.LANCZOS)
    save_png("jungle_path.png", image)


def texture_bark(name, pale=False):
    rng = np.random.default_rng(SEED + (6 if pale else 7))
    size = 512
    detail = size * 2
    # Long vertical furrows at a low frequency, then fine grain on top.
    n = fbm2(size, 24, 5, 4, rng)
    ridge = 1.0 - np.abs(2.0 * n - 1.0)
    ridge = np.power(ridge, 1.45)
    grain = fbm2(size, 10, 30, 3, rng)
    if pale:
        dark = (0.24, 0.21, 0.15)
        mid = (0.50, 0.46, 0.33)
        light = (0.62, 0.58, 0.42)
    else:
        dark = (0.085, 0.060, 0.040)
        mid = (0.25, 0.175, 0.10)
        light = (0.38, 0.29, 0.175)
    rgb = mix_color(dark, mid, ridge)
    rgb = mix_color(rgb, light, np.clip(ridge - 0.58, 0.0, 1.0) * 0.85)
    rgb *= (0.9 + grain * 0.2)[..., None]
    # Lichen patches.
    lichen = fbm2(size, 9, 9, 4, rng)
    mask = np.clip((lichen - 0.55) / 0.18, 0.0, 1.0) * (0.45 if pale else 0.38)
    rgb = mix_color(rgb, (0.31, 0.40, 0.20), mask)
    image = to_image(rgb).resize((detail, detail), Image.LANCZOS)
    draw = ImageDraw.Draw(image)
    for _ in range(120):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        width = rng.uniform(1, 4)
        length = rng.uniform(40, 190)
        drift = rng.uniform(-10, 10)
        color = (26, 18, 11) if not pale else (72, 64, 46)
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.line(
                [(x + dx, y + dy), (x + dx + drift, y + dy + length)],
                fill=color,
                width=int(width),
            ),
        )
    for _ in range(110):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        length = rng.uniform(10, 70)
        drift = rng.uniform(-8, 8)
        color = (24, 18, 12) if not pale else (66, 60, 44)
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.line(
                [(x + dx, y + dy), (x + dx + length, y + dy + drift)],
                fill=color,
                width=int(rng.integers(1, 3)),
            ),
        )
    save_png(name, image.resize((size, size), Image.LANCZOS))


def texture_rock():
    rng = np.random.default_rng(SEED + 8)
    size = 512
    detail = size * 2
    n = fbm2(size, 7, 7, 5, rng)
    rgb = mix_color((0.20, 0.20, 0.19), (0.52, 0.51, 0.47), n)
    cracks = fbm2(size, 20, 16, 3, rng)
    crack_mask = np.clip((0.5 - cracks) * 6.0, 0.0, 1.0) * 0.6
    rgb = mix_color(rgb, (0.10, 0.10, 0.09), crack_mask)
    moss = fbm2(size, 11, 11, 4, rng)
    moss_mask = np.clip((moss - 0.55) / 0.22, 0.0, 1.0) * 0.55
    rgb = mix_color(rgb, (0.18, 0.29, 0.10), moss_mask)
    image = to_image(rgb).resize((detail, detail), Image.LANCZOS)
    draw = ImageDraw.Draw(image)
    for _ in range(80):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        angle = rng.uniform(0, TAU)
        length = rng.uniform(12, 70)
        x2 = x + math.cos(angle) * length
        y2 = y + math.sin(angle) * length
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.line(
                [(x + dx, y + dy), (x2 + dx, y2 + dy)],
                fill=(38, 37, 34),
                width=int(rng.integers(1, 4)),
            ),
        )
    for _ in range(120):
        x = rng.uniform(0, detail)
        y = rng.uniform(0, detail)
        r = rng.uniform(1, 5)
        shade = int(rng.uniform(45, 115))
        wrapped_draw(
            draw,
            detail,
            lambda dx, dy: draw.ellipse(
                (x + dx - r, y + dy - r, x + dx + r, y + dy + r),
                fill=(shade, shade, shade - 3),
            ),
        )
    save_png("jungle_rock.png", image.resize((size, size), Image.LANCZOS))


def texture_water():
    rng = np.random.default_rng(SEED + 9)
    size = 512
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    n = fbm2(size, 3, 2, 4, rng)
    rgb = mix_color((0.018, 0.075, 0.070), (0.060, 0.155, 0.135), n)
    # Broad downstream current plus fine surface texture. No bright speckles:
    # the material's normal map supplies the ripples, and the sun/environment
    # terms supply the highlights.
    flow = (
        np.sin(yy * TAU * 2.0 / size + xx * 0.012)
        + 0.5 * np.sin(yy * TAU * 5.0 / size - xx * 0.028 + 1.7)
        + 0.3 * np.sin(xx * TAU * 3.0 / size + yy * 0.020)
    ) / 1.8
    rgb = mix_color(rgb, (0.10, 0.22, 0.19), np.clip(flow, 0.0, 1.0) * 0.35)
    save_png("jungle_water.png", to_image(np.clip(rgb, 0.0, 1.0)))


def water_normal_map(size=512):
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    height = (
        np.sin(xx * TAU * 2.0 / size + yy * 0.018) * 0.55
        + np.sin(xx * TAU * 4.0 / size - yy * TAU * 2.0 / size + 1.3) * 0.30
        + np.sin(xx * TAU * 7.0 / size + yy * TAU * 4.0 / size + 4.1) * 0.18
        + np.sin(xx * TAU * 11.0 / size - yy * TAU * 7.0 / size + 2.2) * 0.10
    )
    # Sobel gradient, tileable because every wave is periodic in both axes.
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    strength = 3.5
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    normal = np.stack(
        [(nx / length * 0.5 + 0.5) * 255.0,
         (ny / length * 0.5 + 0.5) * 255.0,
         (nz / length * 0.5 + 0.5) * 255.0],
        axis=-1,
    )
    Image.fromarray(np.clip(normal, 0, 255).astype(np.uint8), "RGB").save(
        os.path.join(TEXTURE_DIR, "jungle_water_normal.png")
    )


def texture_cloth(name, base, weave_strength, rng_seed):
    rng = np.random.default_rng(rng_seed)
    size = 256
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    weave = (
        np.sin(xx * TAU * 44.0 / size) * 0.5
        + np.sin(yy * TAU * 44.0 / size) * 0.5
    )
    n = fbm2(size, 8, 8, 4, rng)
    shade = 1.0 + weave * weave_strength + (n - 0.5) * 0.16
    rgb = np.asarray(base, dtype=np.float32)[None, None, :] * shade[..., None]
    rgb += rng.normal(0.0, 0.012, rgb.shape)
    save_png(name, to_image(np.clip(rgb, 0.0, 1.0)))


def texture_leather():
    rng = np.random.default_rng(SEED + 12)
    size = 256
    n = fbm2(size, 12, 12, 5, rng)
    rgb = mix_color((0.16, 0.095, 0.055), (0.40, 0.26, 0.15), n)
    creases = fbm2(size, 22, 8, 3, rng)
    mask = np.clip((0.42 - creases) * 4.0, 0.0, 1.0) * 0.5
    rgb = mix_color(rgb, (0.10, 0.06, 0.035), mask)
    save_png("jungle_leather.png", to_image(rgb))


def texture_skin():
    rng = np.random.default_rng(SEED + 13)
    size = 256
    n = fbm2(size, 6, 6, 4, rng)
    rgb = mix_color((0.52, 0.34, 0.23), (0.74, 0.53, 0.38), n)
    rgb += rng.normal(0.0, 0.010, rgb.shape)
    save_png("jungle_skin.png", to_image(np.clip(rgb, 0.0, 1.0)))


def texture_canvas():
    rng = np.random.default_rng(SEED + 14)
    size = 256
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    weave = (
        np.sin(xx * TAU * 60.0 / size) * 0.5
        + np.sin(yy * TAU * 60.0 / size) * 0.5
    )
    n = fbm2(size, 7, 7, 4, rng)
    rgb = mix_color((0.47, 0.42, 0.29), (0.68, 0.63, 0.46), n)
    rgb *= (1.0 + weave * 0.05 + (n - 0.5) * 0.10)[..., None]
    save_png("jungle_canvas.png", to_image(np.clip(rgb, 0.0, 1.0)))


def texture_metal():
    rng = np.random.default_rng(SEED + 15)
    size = 128
    n = fbm2(size, 40, 4, 3, rng)
    rgb = mix_color((0.30, 0.31, 0.33), (0.64, 0.66, 0.68), n)
    scratches = (rng.random((size, size)) > 0.985).astype(np.float32)
    rgb = mix_color(rgb, (0.80, 0.82, 0.84), scratches * 0.6)
    save_png("jungle_metal.png", to_image(rgb))


WING_CELLS = {
    "dragonfly": (0, 0, 512, 512),
    "monarch": (512, 0, 512, 512),
    "morpho": (0, 512, 512, 512),
    "bee": (512, 512, 512, 512),
}


def texture_insect_wings():
    rng = np.random.default_rng(SEED + 50)
    size = 1024
    image = Image.new("RGB", (size, size), (230, 235, 230))
    draw = ImageDraw.Draw(image)
    for species, (x0, y0, w, h) in WING_CELLS.items():
        cx = x0 + w * 0.08
        cy = y0 + h * 0.5
        if species == "dragonfly":
            base = (138, 174, 188)
            vein = (22, 36, 42)
            edge = (58, 80, 90)
        elif species == "monarch":
            base = (214, 92, 20)
            vein = (24, 18, 14)
            edge = (20, 14, 10)
        elif species == "morpho":
            base = (24, 58, 172)
            vein = (10, 14, 30)
            edge = (8, 10, 20)
        else:
            base = (224, 215, 174)
            vein = (54, 48, 34)
            edge = (96, 82, 56)
        for yy in range(y0, y0 + h, 2):
            t = (yy - y0) / h
            shade = 0.92 + 0.08 * math.sin(t * 12.0)
            color = tuple(int(min(255, c * shade)) for c in base)
            draw.line([(x0, yy), (x0 + w, yy)], fill=color, width=2)
        # Leading/trailing edges.
        draw.line([(x0, y0 + 8), (x0 + w, y0 + 8)], fill=edge, width=10)
        draw.line([(x0, y0 + h - 8), (x0 + w, y0 + h - 8)], fill=edge, width=8)
        # Radiating veins.
        for i in range(14):
            ty = y0 + h * (0.05 + 0.9 * (i / 13.0))
            draw.line([(cx, cy), (x0 + w * 0.96, ty)], fill=vein, width=2)
        # Cross veins.
        for i in range(1, 8):
            xx = x0 + w * (0.10 + 0.80 * i / 8.0)
            draw.line([(xx, y0 + h * 0.10), (xx + w * 0.04, y0 + h * 0.90)],
                      fill=vein, width=1)
        if species == "dragonfly":
            for yy in (y0 + h * 0.22, y0 + h * 0.78):
                draw.ellipse([x0 + w * 0.78, yy - 14, x0 + w * 0.88, yy + 14],
                             fill=(28, 40, 44))
        if species == "monarch":
            draw.rectangle([x0, y0, x0 + w - 1, y0 + h - 1], outline=(12, 10, 8), width=26)
            for _ in range(30):
                px = x0 + rng.uniform(0.06, 0.94) * w
                py = y0 + rng.uniform(0.06, 0.94) * h
                if px > x0 + 30 and px < x0 + w - 30 and py > y0 + 30 and py < y0 + h - 30:
                    continue
                r = rng.uniform(2, 6)
                draw.ellipse([px - r, py - r, px + r, py + r], fill=(238, 228, 202))
        elif species == "morpho":
            draw.rectangle([x0, y0, x0 + w - 1, y0 + h - 1], outline=(4, 6, 14), width=42)
            for i in range(6):
                yy = y0 + h * (0.18 + i * 0.11)
                draw.line([(x0 + 26, yy), (x0 + w - 26, yy + 10)],
                          fill=(42, 100, 220), width=4)
        elif species == "bee":
            for i in range(8):
                xx = x0 + w * (0.12 + i * 0.10)
                draw.line([(xx, y0 + 12), (xx - 6, y0 + h - 12)],
                          fill=(70, 60, 40), width=2)
    image.save(os.path.join(TEXTURE_DIR, "jungle_insect_wings.png"))
    print("  texture jungle_insect_wings.png 1024x1024")


def texture_bark_smooth():
    rng = np.random.default_rng(SEED + 52)
    size = 512
    n = fbm2(size, 10, 10, 5, rng)
    rgb = mix_color((0.26, 0.24, 0.20), (0.55, 0.51, 0.42), n)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    # Horizontal lenticels and shallow cracks.
    for y in range(10, size, 37):
        wobble = np.sin(xx * 0.021 + y * 0.13) * 4.0
        band = np.exp(-((yy - (y + wobble)) / 2.2) ** 2)
        rgb = mix_color(rgb, (0.16, 0.15, 0.12), band * 0.35)
    for _ in range(28):
        x = rng.uniform(0, size)
        y = rng.uniform(0, size)
    # Moss and lichen.
    lichen = fbm2(size, 7, 7, 4, rng)
    mask = np.clip((lichen - 0.52) / 0.2, 0.0, 1.0) * 0.35
    rgb = mix_color(rgb, (0.30, 0.40, 0.20), mask)
    arr = np.clip(rgb * 255.0 + rng.normal(0.0, 1.6, (size, size, 3)), 0, 255)
    image = to_image(arr / 255.0)
    draw = ImageDraw.Draw(image)
    for _ in range(35):
        x = rng.uniform(0, size)
        y = rng.uniform(0, size)
        draw.line([(x, y), (x + rng.uniform(12, 60), y + rng.uniform(-3, 3))],
                  fill=(78, 68, 52), width=1)
    save_png("jungle_bark_smooth.png", image)


def texture_palm_bark():
    rng = np.random.default_rng(SEED + 51)
    size = 512
    n = fbm2(size, 40, 6, 4, rng)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    fiber = 0.5 + 0.5 * np.sin(xx * 0.95 + n * 9.0)
    rgb = mix_color((0.26, 0.25, 0.20), (0.56, 0.53, 0.42), n)
    rgb *= (0.9 + 0.2 * fiber)[..., None]
    # Ring scars.
    for y in range(8, size, 34):
        wobble = np.sin(xx * 0.035 + y * 0.2) * 3.0
        band = np.exp(-((yy - (y + wobble)) / 3.0) ** 2)
        rgb = mix_color(rgb, (0.16, 0.145, 0.11), band * 0.55)
    # Lichen and dirt.
    lichen = fbm2(size, 8, 8, 4, rng)
    mask = np.clip((lichen - 0.58) / 0.16, 0.0, 1.0) * 0.3
    rgb = mix_color(rgb, (0.30, 0.40, 0.20), mask)
    arr = np.clip(rgb * 255.0 + rng.normal(0.0, 2.0, (size, size, 3)), 0, 255)
    save_png("jungle_palm_bark.png", to_image(arr / 255.0))


NORMAL_DEFS = [
    ("jungle_ground", "jungle_ground.png", 2.2, 1.1),
    ("jungle_path_tex", "jungle_path.png", 1.8, 0.8),
    ("jungle_leaf_grain", "jungle_leaf_grain.png", 1.2, 0.5),
    ("jungle_leaf_grain_light", "jungle_leaf_grain_light.png", 1.2, 0.5),
    ("jungle_bark", "jungle_bark.png", 4.5, 1.0),
    ("jungle_bark_pale", "jungle_bark_pale.png", 3.2, 1.0),
    ("jungle_bark_smooth", "jungle_bark_smooth.png", 2.2, 1.0),
    ("jungle_palm_bark", "jungle_palm_bark.png", 3.0, 0.8),
    ("jungle_rock", "jungle_rock.png", 3.6, 1.0),
    ("jungle_water", "jungle_water.png", 2.8, 0.6),
    ("jungle_avatar_atlas", "jungle_avatar_atlas.png", 1.0, 0.4),
    ("jungle_insect_wings", "jungle_insect_wings.png", 1.6, 0.4),
]


def generate_normal_map(source_path, out_path, strength, blur_radius):
    source = Image.open(source_path).convert("RGB")
    if blur_radius > 0.0:
        source = source.filter(ImageFilter.GaussianBlur(blur_radius))
    rgb = np.asarray(source).astype(np.float32) / 255.0
    height = (
        rgb[:, :, 0] * 0.299 + rgb[:, :, 1] * 0.587 + rgb[:, :, 2] * 0.114
    )
    # Sobel, wrapping at the border so tiled surfaces stay seamless.
    dx = (
        np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)
    ) * 0.5
    dy = (
        np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)
    ) * 0.5
    nx = -dx * strength
    ny = -dy * strength
    nz = np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx /= length
    ny /= length
    nz /= length
    normal = np.stack(
        [
            (nx * 0.5 + 0.5) * 255.0,
            (ny * 0.5 + 0.5) * 255.0,
            (nz * 0.5 + 0.5) * 255.0,
        ],
        axis=-1,
    )
    Image.fromarray(np.clip(normal, 0, 255).astype(np.uint8), "RGB").save(
        out_path
    )


def build_normal_maps():
    print("normal maps:")
    for key, filename, strength, blur in NORMAL_DEFS:
        source = os.path.join(TEXTURE_DIR, filename)
        out = os.path.join(TEXTURE_DIR, key + "_normal.png")
        generate_normal_map(source, out, strength, blur)
        print(f"  {key}_normal.png")


def build_textures():
    os.makedirs(TEXTURE_DIR, exist_ok=True)
    print("textures:")
    texture_ground()
    texture_path()
    texture_leaf_grain("jungle_leaf_grain.png", bright=False)
    texture_leaf_grain("jungle_leaf_grain_light.png", bright=True)
    texture_bark("jungle_bark.png", pale=False)
    texture_bark("jungle_bark_pale.png", pale=True)
    texture_rock()
    texture_water()
    texture_canvas()
    texture_insect_wings()
    texture_palm_bark()
    texture_bark_smooth()
    build_normal_maps()
    water_normal_map()


# --------------------------------------------------------------------------
# GLB meshes
# --------------------------------------------------------------------------


def write_glb(path, name, positions, normals, uvs, indices):
    positions = np.asarray(positions, dtype=np.float32)
    normals = np.asarray(normals, dtype=np.float32)
    uvs = np.asarray(uvs, dtype=np.float32)
    indices = np.asarray(indices, dtype=np.uint32)

    def pad4(data):
        while len(data) % 4:
            data += b"\x00"
        return data

    pos_bytes = positions.tobytes()
    nrm_bytes = normals.tobytes()
    uv_bytes = uvs.tobytes()
    idx_bytes = indices.tobytes()
    offsets = []
    blob = b""
    for chunk in (pos_bytes, nrm_bytes, uv_bytes, idx_bytes):
        offsets.append(len(blob))
        blob += pad4(chunk)

    views = [
        {"buffer": 0, "byteOffset": offsets[0], "byteLength": len(pos_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[1], "byteLength": len(nrm_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[2], "byteLength": len(uv_bytes), "target": 34962},
        {"buffer": 0, "byteOffset": offsets[3], "byteLength": len(idx_bytes), "target": 34963},
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
        {
            "bufferView": 1,
            "componentType": 5126,
            "count": int(normals.shape[0]),
            "type": "VEC3",
        },
        {
            "bufferView": 2,
            "componentType": 5126,
            "count": int(uvs.shape[0]),
            "type": "VEC2",
        },
        {
            "bufferView": 3,
            "componentType": 5125,
            "count": int(indices.shape[0]),
            "type": "SCALAR",
        },
    ]
    gltf = {
        "asset": {"version": "2.0", "generator": "lotus make_jungle.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": name}],
        "meshes": [
            {
                "name": name,
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
    print(
        f"  mesh {os.path.basename(path)} "
        f"{len(positions)} verts {len(indices)//3} tris"
    )


def build_terrain_mesh():
    size = 120.0
    segments = 48
    step = size / segments
    half = size * 0.5
    positions = []
    normals = []
    uvs = []
    for iz in range(segments + 1):
        z = -half + iz * step
        for ix in range(segments + 1):
            x = -half + ix * step
            y = terrain_height(x, z)
            positions.append((x, y, z))
            e = 0.6
            dx = (terrain_height(x + e, z) - terrain_height(x - e, z)) / (2 * e)
            dz = (terrain_height(x, z + e) - terrain_height(x, z - e)) / (2 * e)
            length = math.sqrt(dx * dx + 1.0 + dz * dz)
            normals.append((-dx / length, 1.0 / length, -dz / length))
            uvs.append((x / 4.0, z / 4.0))
    indices = []
    stride = segments + 1
    for iz in range(segments):
        for ix in range(segments):
            a = iz * stride + ix
            b = a + 1
            c = a + stride
            d = c + 1
            indices.extend((a, c, b, b, c, d))
    write_glb(
        os.path.join(ROOT, "assets", "jungle_terrain.glb"),
        "JungleTerrain",
        positions,
        normals,
        uvs,
        indices,
    )


def build_path_mesh():
    width = 0.65
    positions = []
    normals = []
    uvs = []
    indices = []
    bridge_z = bridge_center()[1]
    runs = [(24.0, bridge_z + 3.4), (bridge_z - 3.4, -34.0)]
    distance = 0.0
    for run_index, (z0, z1) in enumerate(runs):
        steps = max(8, int(abs(z1 - z0) / 0.5))
        base = len(positions)
        for i in range(steps + 1):
            t = i / steps
            z = lerp(z0, z1, t)
            xc = path_center(z)
            dz = 0.01
            tangent = (path_center(z + dz) - path_center(z - dz), 2 * dz)
            tl = math.hypot(*tangent)
            tx, tz = tangent[0] / tl, tangent[1] / tl
            nx, nz = tz, -tx
            for side in (-1.0, 1.0):
                x = xc + nx * width * side
                zz = z + nz * width * side
                y = terrain_height(x, zz) + 0.025
                positions.append((x, y, zz))
                normals.append((0.0, 1.0, 0.0))
                uvs.append((0.5 + 0.5 * side, distance / 2.4))
            distance += 0.5
        for i in range(steps):
            a = base + i * 2
            b = a + 1
            c = a + 2
            d = a + 3
            indices.extend((a, b, c, b, d, c))
    write_glb(
        os.path.join(ROOT, "assets", "jungle_path.glb"),
        "JunglePath",
        positions,
        normals,
        uvs,
        indices,
    )


def build_stream_mesh():
    width = 1.45
    positions = []
    normals = []
    uvs = []
    indices = []
    x0, x1 = -38.0, 38.0
    steps = 240
    distance = 0.0
    for i in range(steps + 1):
        t = i / steps
        x = lerp(x0, x1, t)
        zc = stream_center(x)
        dz = 0.01
        tangent = (2 * dz, stream_center(x + dz) - stream_center(x - dz))
        tl = math.hypot(*tangent)
        tx, tz = tangent[0] / tl, tangent[1] / tl
        nx, nz = tz, -tx
        for side in (-1.0, 1.0):
            positions.append(
                (x + nx * width * side, WATER_LEVEL, zc + nz * width * side)
            )
            normals.append((0.0, 1.0, 0.0))
            uvs.append((0.5 + 0.5 * side, distance / 3.0))
        distance += (78.0 / steps) * 1.02
    for i in range(steps):
        a = i * 2
        b = a + 1
        c = a + 2
        d = a + 3
        indices.extend((a, b, c, b, d, c))
    write_glb(
        os.path.join(ROOT, "assets", "jungle_stream.glb"),
        "JungleStream",
        positions,
        normals,
        uvs,
        indices,
    )


def bridge_center():
    z = stream_center(0.0)
    x = path_center(z)
    return x, z


# --------------------------------------------------------------------------
# geometry-leaf meshes (canopy clusters, ferns, palm fronds, grass)
# --------------------------------------------------------------------------


def cell_uv(index, inset=0.022):
    col = index % 4
    row = index // 4
    u0 = col * 0.25 + inset
    u1 = (col + 1) * 0.25 - inset
    v0 = row * 0.25 + inset
    v1 = (row + 1) * 0.25 - inset
    return (u0, v0, u1, v1)


LEAF_SHAPE = (
    (0.0, -0.5),
    (-0.48, -0.24),
    (-0.44, 0.16),
    (0.0, 0.5),
    (0.44, 0.16),
    (0.48, -0.24),
)


def add_leaf(positions, normals, uvs, indices, center, tangent, bitangent,
             length, width, normal, fold=0.03):
    """A six-vertex pointed leaf with the grain texture mapped across it."""
    center = np.asarray(center, dtype=np.float64)
    tangent = np.asarray(tangent, dtype=np.float64)
    bitangent = np.asarray(bitangent, dtype=np.float64)
    normal = np.asarray(normal, dtype=np.float64)
    tangent /= np.linalg.norm(tangent)
    bitangent /= np.linalg.norm(bitangent)
    normal /= np.linalg.norm(normal)
    base = len(positions)
    for x, y in LEAF_SHAPE:
        point = center + tangent * (y * length) + bitangent * (x * width)
        point = point + normal * (fold * (1.0 - abs(y) * 2.0))
        positions.append(tuple(float(v) for v in point))
        normals.append(tuple(float(v) for v in normal))
        uvs.append((0.5 + x * 0.92, 0.5 + y * 0.92))
    for a, b in ((1, 2), (2, 3), (3, 4), (4, 5)):
        indices.extend((base, base + a, base + b))


def add_simple_leaf(positions, normals, uvs, indices, center, tangent,
                    bitangent, length, width, normal):
    center = np.asarray(center, dtype=np.float64)
    tangent = np.asarray(tangent, dtype=np.float64) / max(
        np.linalg.norm(tangent), 1e-9
    )
    bitangent = np.asarray(bitangent, dtype=np.float64) / max(
        np.linalg.norm(bitangent), 1e-9
    )
    normal = np.asarray(normal, dtype=np.float64) / max(
        np.linalg.norm(normal), 1e-9
    )
    base = len(positions)
    points = (
        center - tangent * (length * 0.5),
        center + bitangent * (width * 0.5),
        center + tangent * (length * 0.5),
        center - bitangent * (width * 0.5),
    )
    for point in points:
        positions.append(tuple(float(v) for v in point))
        normals.append(tuple(float(v) for v in normal))
    uvs.extend(((0.5, 0.0), (0.0, 0.5), (0.5, 1.0), (1.0, 0.5)))
    indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))


def quad(positions, normals, uvs, indices, p0, p1, p2, p3, uvrect, normal=None):
    if normal is None:
        edge1 = np.asarray(p1, dtype=np.float64) - np.asarray(p0, dtype=np.float64)
        edge2 = np.asarray(p2, dtype=np.float64) - np.asarray(p0, dtype=np.float64)
        n = np.cross(edge1, edge2)
        length = np.linalg.norm(n)
        normal = tuple(n / length) if length > 1e-9 else (0.0, 1.0, 0.0)
    base = len(positions)
    for point in (p0, p1, p2, p3):
        positions.append(tuple(float(v) for v in point))
        normals.append(tuple(float(v) for v in normal))
    u0, v0, u1, v1 = uvrect
    uvs.extend(((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))


def build_trunk_mesh():
    rng = np.random.default_rng(SEED + 30)
    segments = 22
    rings = [
        (0.0, 0.56),
        (0.14, 0.49),
        (0.36, 0.39),
        (0.64, 0.32),
        (1.0, 0.255),
    ]
    positions = []
    normals = []
    uvs = []
    indices = []
    for ring_index, (y, radius) in enumerate(rings):
        bend = 0.02 * math.sin(y * 5.0 + 0.7)
        for i in range(segments + 1):
            angle = TAU * i / segments
            c = math.cos(angle)
            s = math.sin(angle)
            flute = 1.0 + 0.035 * math.sin(angle * 3.0 + ring_index * 1.7)
            r = radius * flute
            positions.append((c * r + bend, y, s * r))
            normals.append((c, 0.18, s))
            uvs.append((i / segments, y * 9.0))
    stride = segments + 1
    for ring in range(len(rings) - 1):
        for i in range(segments):
            a = ring * stride + i
            b = (ring + 1) * stride + i
            indices.extend((a, a + 1, b, a + 1, b + 1, b))
    # Buttress fins: four vertical blades that blend the trunk into the floor.
    for fin in range(4):
        angle = TAU * fin / 4 + 0.35
        c = math.cos(angle)
        s = math.sin(angle)
        tang = (-s, 0.0, c)
        inner_high = (c * 0.40, 0.34, s * 0.40)
        outer_low = (c * 1.02, 0.0, s * 1.02)
        outer_mid = (c * 1.02, 0.10, s * 1.02)
        inner_low = (c * 0.50, 0.0, s * 0.50)
        normal = (tang[2] * 0.45, 1.0, -tang[0] * 0.45)
        n = np.asarray(normal, dtype=np.float64)
        normal = tuple(n / np.linalg.norm(n))
        quad(
            positions,
            normals,
            uvs,
            indices,
            inner_high,
            outer_mid,
            outer_low,
            inner_low,
            (0.0, 0.0, 0.35, 0.30),
            normal,
        )
        # Thin second face gives the fin a little volume in silhouette.
        quad(
            positions,
            normals,
            uvs,
            indices,
            inner_low,
            outer_low,
            outer_mid,
            inner_high,
            (0.0, 0.0, 0.35, 0.30),
            normal,
        )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_trunk.glb"),
        "JungleTrunk",
        positions,
        normals,
        uvs,
        indices,
    )


def build_palm_trunk_mesh():
    segments = 18
    rings = 16
    positions = []
    normals = []
    uvs = []
    indices = []
    for ring in range(rings + 1):
        t = ring / rings
        bend = 0.05 * math.sin(t * 2.0)
        radius = (0.52 * (1.0 - 0.42 * t)) * (
            1.0 + 0.035 * math.sin(t * 52.0)
        )
        for seg in range(segments + 1):
            theta = -PI + TAU * seg / segments
            c = math.cos(theta)
            s = math.sin(theta)
            positions.append((bend + c * radius, t, s * radius))
            normals.append((c, 0.1, s))
            uvs.append((seg / segments, t * 4.0))
    stride = segments + 1
    for ring in range(rings):
        for seg in range(segments):
            a = ring * stride + seg
            b = (ring + 1) * stride + seg
            indices.extend((a, b, a + 1, a + 1, b, b + 1))
    write_glb(
        os.path.join(ROOT, "assets", "jungle_palm_trunk.glb"),
        "JunglePalmTrunk",
        positions,
        normals,
        uvs,
        indices,
    )


def build_canopy_mesh(name, seed_offset, count=1200,
                      length_range=(0.050, 0.115),
                      width_range=(0.44, 0.68)):
    rng = np.random.default_rng(SEED + seed_offset)
    positions = []
    normals = []
    uvs = []
    indices = []
    for _ in range(count):
        direction = rng.normal(size=3)
        direction /= np.linalg.norm(direction)
        radius = rng.uniform(0.15, 1.0) ** 0.55
        center = direction * radius
        center[1] *= 0.72
        n = direction * 0.6 + rng.normal(size=3) * 0.4
        n /= np.linalg.norm(n)
        # Foliage scatters light; bias leaf normals upward so back faces do not
        # read as black cards when the camera looks up into the canopy.
        n = n * 0.55 + np.array([0.0, 1.0, 0.0]) * 0.45
        n /= np.linalg.norm(n)
        if n[1] < 0.15:
            n[1] = 0.15
            n /= np.linalg.norm(n)
        helper = np.array([0.0, 1.0, 0.0]) if abs(n[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
        tangent = np.cross(n, helper)
        tangent /= np.linalg.norm(tangent)
        bitangent = np.cross(n, tangent)
        length = rng.uniform(*length_range)
        width = length * rng.uniform(*width_range)
        fold = n * 0.025
        add_simple_leaf(
            positions,
            normals,
            uvs,
            indices,
            center,
            tangent,
            bitangent,
            length,
            width,
            n,
        )
    path = os.path.join(ROOT, "assets", name)
    write_glb(path, os.path.splitext(name)[0], positions, normals, uvs, indices)


def build_fern_mesh():
    rng = np.random.default_rng(SEED + 34)
    positions = []
    normals = []
    uvs = []
    indices = []
    fronds = 11
    for frond in range(fronds):
        yaw = TAU * frond / fronds + rng.uniform(-0.18, 0.18)
        dx = math.cos(yaw)
        dz = math.sin(yaw)
        side = (-dz, 0.0, dx)
        rachis_points = []
        for step in range(9):
            t = step / 8.0
            radial = 0.08 + t * 0.92
            y = 1.10 * t - 0.55 * t * t
            rachis_points.append((dx * radial, y, dz * radial))
        for step in range(8):
            a = rachis_points[step]
            b = rachis_points[step + 1]
            rachis_dir = np.asarray(b) - np.asarray(a)
            rachis_dir /= np.linalg.norm(rachis_dir)
            normal = (side[2], 0.5, -side[0])
            quad(
                positions,
                normals,
                uvs,
                indices,
                (a[0] - side[0] * 0.016, a[1], a[2] - side[2] * 0.016),
                (a[0] + side[0] * 0.016, a[1], a[2] + side[2] * 0.016),
                (b[0] + side[0] * 0.012, b[1], b[2] + side[2] * 0.012),
                (b[0] - side[0] * 0.012, b[1], b[2] - side[2] * 0.012),
                cell_uv(int(rng.integers(0, 16))),
                normal,
            )
        for step in range(1, 8):
            t = step / 8.0
            base = rachis_points[step]
            for sign in (-1.0, 1.0):
                angle = sign * rng.uniform(0.85, 1.20)
                ca = math.cos(angle)
                sa = math.sin(angle)
                ldx = dx * ca - dz * sa
                ldz = dx * sa + dz * ca
                length = (0.17 * (1.0 - t) + 0.028) * rng.uniform(0.9, 1.1)
                tip = (
                    base[0] + ldx * length,
                    base[1] + 0.035 + rng.uniform(-0.015, 0.025),
                    base[2] + ldz * length,
                )
                center = (
                    (base[0] + tip[0]) * 0.5,
                    (base[1] + tip[1]) * 0.5,
                    (base[2] + tip[2]) * 0.5,
                )
                add_leaf(
                    positions,
                    normals,
                    uvs,
                    indices,
                    center,
                    (ldx, 0.12, ldz),
                    (-ldz, 0.0, ldx),
                    length * 1.9,
                    length * 0.85,
                    (0.0, 1.0, 0.0),
                    fold=0.01,
                )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_fern.glb"),
        "JungleFern",
        positions,
        normals,
        uvs,
        indices,
    )


def build_broadleaf_mesh():
    rng = np.random.default_rng(SEED + 35)
    positions = []
    normals = []
    uvs = []
    indices = []
    for i in range(12):
        yaw = TAU * i / 12 + rng.uniform(-0.25, 0.25)
        dx = math.cos(yaw)
        dz = math.sin(yaw)
        side = (-dz, 0.0, dx)
        length = rng.uniform(0.62, 0.92)
        width = rng.uniform(0.16, 0.26)
        up = rng.uniform(0.35, 0.85)
        center = (
            dx * length * 0.5,
            0.16 + up * length * 0.5,
            dz * length * 0.5,
        )
        add_leaf(
            positions,
            normals,
            uvs,
            indices,
            center,
            (dx, up, dz),
            side,
            length,
            width,
            (-dx * 0.2, 0.95, -dz * 0.2),
            fold=0.035,
        )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_broadleaf.glb"),
        "JungleBroadleaf",
        positions,
        normals,
        uvs,
        indices,
    )


def build_banana_crown_mesh():
    positions = []
    normals = []
    uvs = []
    indices = []
    leaves = 6
    rows = 6
    for leaf in range(leaves):
        yaw = TAU * leaf / leaves + 0.2
        dx = math.cos(yaw)
        dz = math.sin(yaw)
        side = (-dz, 0.0, dx)
        previous_top = None
        previous_bottom = None
        for row in range(rows + 1):
            t = row / rows
            radial = 0.05 + 0.72 * t
            y = 1.45 * t - 1.55 * t * t
            half = 0.175 * max(0.0, math.sin(PI * min(1.0, t * 1.02))) ** 0.62 + 0.010
            if t > 0.92:
                half *= max(0.05, (1.0 - t) / 0.08)
            cx = dx * radial
            cz = dz * radial
            fold = 0.055 * math.sin(PI * t)
            top = (cx + side[0] * half, y + fold, cz + side[2] * half)
            bottom = (cx - side[0] * half, y + fold, cz - side[2] * half)
            if previous_top is not None:
                a = len(positions)
                for p, uv in ((previous_top, (0.0, (row - 1) / rows)),
                              (previous_bottom, (1.0, (row - 1) / rows)),
                              (top, (0.0, row / rows)),
                              (bottom, (1.0, row / rows))):
                    positions.append(p)
                    normals.append((0.0, 1.0, 0.0))
                    uvs.append(uv)
                indices.extend((a, a + 2, a + 1, a + 1, a + 2, a + 3))
            previous_top = top
            previous_bottom = bottom
    write_glb(
        os.path.join(ROOT, "assets", "jungle_banana_crown.glb"),
        "JungleBananaCrown",
        positions,
        normals,
        uvs,
        indices,
    )


def build_grass_mesh():
    rng = np.random.default_rng(SEED + 36)
    positions = []
    normals = []
    uvs = []
    indices = []
    for i in range(34):
        yaw = rng.uniform(0, TAU)
        dx = math.cos(yaw)
        dz = math.sin(yaw)
        side = (-dz, 0.0, dx)
        height = rng.uniform(0.45, 1.0)
        lean = rng.uniform(0.08, 0.35)
        base = (rng.uniform(-0.08, 0.08), 0.0, rng.uniform(-0.08, 0.08))
        center = (
            base[0] + dx * lean * 0.5,
            height * 0.5,
            base[2] + dz * lean * 0.5,
        )
        add_leaf(
            positions,
            normals,
            uvs,
            indices,
            center,
            (dx * lean, 1.0, dz * lean),
            side,
            height,
            rng.uniform(0.025, 0.055),
            (side[0] * 0.2, 0.85, side[2] * 0.2),
            fold=0.0,
        )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_grass.glb"),
        "JungleGrass",
        positions,
        normals,
        uvs,
        indices,
    )


def build_palm_frond_mesh():
    rng = np.random.default_rng(SEED + 37)
    positions = []
    normals = []
    uvs = []
    indices = []

    def curve(t):
        return (0.0, 0.55 * t - 0.95 * t * t, t)

    for step in range(10):
        t0 = step / 10.0
        t1 = (step + 1) / 10.0
        a = curve(t0)
        b = curve(t1)
        quad(
            positions,
            normals,
            uvs,
            indices,
            (-0.018, a[1], a[2]),
            (0.018, a[1], a[2]),
            (0.012, b[1], b[2]),
            (-0.012, b[1], b[2]),
            cell_uv(int(rng.integers(0, 16))),
            (0.0, 1.0, 0.0),
        )
    for step in range(1, 14):
        t = step / 14.0
        base = curve(t)
        for sign in (-1.0, 1.0):
            length = (0.34 * (1.0 - 0.55 * t) + 0.03) * rng.uniform(0.9, 1.1)
            tip = (
                sign * length,
                base[1] + 0.05 - 0.10 * t,
                base[2] + length * 0.45,
            )
            center = (tip[0] * 0.5, (base[1] + tip[1]) * 0.5, (base[2] + tip[2]) * 0.5)
            add_leaf(
                positions,
                normals,
                uvs,
                indices,
                center,
                (sign * 1.0, 0.15, 0.5),
                (0.0, 1.0, 0.0),
                length * 1.8,
                length * 0.55,
                (0.0, 1.0, 0.0),
                fold=0.01,
            )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_palm_frond.glb"),
        "JunglePalmFrond",
        positions,
        normals,
        uvs,
        indices,
    )


def build_leaf_meshes():
    build_trunk_mesh()
    build_palm_trunk_mesh()
    build_canopy_mesh("jungle_canopy.glb", 31)
    build_canopy_mesh("jungle_canopy_sparse.glb", 32)
    build_canopy_mesh(
        "jungle_canopy_broad.glb", 38, count=700,
        length_range=(0.090, 0.195), width_range=(0.56, 0.84),
    )
    build_canopy_mesh(
        "jungle_canopy_fine.glb", 39, count=1400,
        length_range=(0.035, 0.080), width_range=(0.46, 0.72),
    )
    build_fern_mesh()
    build_broadleaf_mesh()
    build_banana_crown_mesh()
    build_grass_mesh()
    build_palm_frond_mesh()


def build_leaf_tuft_mesh():
    rng = np.random.default_rng(SEED + 40)
    positions = []
    normals = []
    uvs = []
    indices = []
    for _ in range(90):
        direction = rng.normal(size=3)
        direction /= np.linalg.norm(direction)
        center = direction * (rng.uniform(0.15, 1.0) ** 0.55)
        center[1] *= 0.7
        n = direction * 0.6 + rng.normal(size=3) * 0.4
        n /= np.linalg.norm(n)
        helper = np.array([0.0, 1.0, 0.0]) if abs(n[1]) < 0.9 else np.array([1.0, 0.0, 0.0])
        tangent = np.cross(n, helper)
        tangent /= np.linalg.norm(tangent)
        bitangent = np.cross(n, tangent)
        add_simple_leaf(
            positions, normals, uvs, indices, center, tangent, bitangent,
            rng.uniform(0.12, 0.24), rng.uniform(0.06, 0.14), n,
        )
    write_glb(
        os.path.join(ROOT, "assets", "jungle_leaf_tuft.glb"),
        "JungleLeafTuft",
        positions,
        normals,
        uvs,
        indices,
    )


WING_MESHES = {
    (species, side): f"assets/jungle_wing_{species}_{'r' if side > 0 else 'l'}.glb"
    for species in ("dragonfly", "monarch", "morpho", "bee")
    for side in (1, -1)
}


def build_insect_wings():
    for species, (x0, y0, cw, ch) in WING_CELLS.items():
        if species == "dragonfly":
            length, half_width = 0.17, 0.028
        elif species == "butterfly":
            continue
        elif species == "bee":
            length, half_width = 0.075, 0.018
        else:
            length, half_width = 0.13, 0.058
        for side in (1, -1):
            positions = []
            normals = []
            uvs = []
            indices = []
            rows = 18
            for row in range(rows + 1):
                t = row / rows
                if species == "dragonfly":
                    half = half_width * (0.18 + 0.82 * math.sin(PI * t) ** 0.55)
                elif species == "bee":
                    half = half_width * (0.20 + 0.80 * math.sin(PI * t) ** 0.65)
                else:
                    # Butterfly: two broad lobes with a soft waist.
                    half = half_width * (0.26 + 0.74 * math.sin(PI * t) ** 0.45) * (
                        0.82 + 0.18 * math.cos(TAU * t)
                    )
                x = side * length * t
                y = 0.012 * math.sin(PI * t)
                u = t if side > 0 else 1.0 - t
                uv_top = ((x0 + u * cw) / 1024.0, (y0 + ch * 0.05) / 1024.0)
                uv_bottom = ((x0 + u * cw) / 1024.0, (y0 + ch * 0.95) / 1024.0)
                positions.append((x, y, half))
                positions.append((x, y, -half))
                normals.append((0.0, 1.0, 0.0))
                normals.append((0.0, 1.0, 0.0))
                uvs.append(uv_top)
                uvs.append(uv_bottom)
            for row in range(rows):
                a = row * 2
                b = a + 1
                c = a + 2
                d = a + 3
                indices.extend((a, c, b, b, c, d))
            write_glb(
                os.path.join(ROOT, "assets", f"jungle_wing_{species}_{'r' if side > 0 else 'l'}.glb"),
                f"JungleWing_{species}",
                positions,
                normals,
                uvs,
                indices,
            )


# --------------------------------------------------------------------------
# scene graph
# --------------------------------------------------------------------------


GUID_COUNTER = 0x200
ROOTS = []
GUID_INDEX = {}


def next_guid():
    global GUID_COUNTER
    value = GUID_COUNTER
    GUID_COUNTER += 1
    return f"{value:x}"


def make_node(name, position=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    node = {
        "name": name,
        "guid": next_guid(),
        "components": {
            "SceneGuid": "",
            "Transform": {
                "position": [round(v, 4) for v in position],
                "rotation": [round(v, 5) for v in rotation],
                "scale": [round(v, 5) for v in scale],
            },
        },
        "children": [],
    }
    node["components"]["SceneGuid"] = node["guid"]
    GUID_INDEX[name] = node["guid"]
    return node


def attach(parent, child):
    if parent is None:
        ROOTS.append(child)
    else:
        parent["children"].append(child)
    return child


NORMAL_MAP_OF_ALBEDO = {
    key: key + "_normal" for key, _file, _strength, _blur in NORMAL_DEFS
}


def material(albedo=None, roughness=0.9, tint=(1.0, 1.0, 1.0),
             normal="_auto", metallic=0.0, emissive=None):
    out = {"base_color": [round(v, 3) for v in tint],
           "roughness": round(roughness, 3)}
    if albedo:
        out["albedo_id"] = albedo
    if normal == "_auto":
        normal = NORMAL_MAP_OF_ALBEDO.get(albedo)
    if normal:
        out["normal_id"] = normal
    if metallic > 0.0:
        out["metallic"] = round(metallic, 3)
    if emissive is not None and any(emissive):
        out["emissive"] = [round(v, 3) for v in emissive]
    return out


def add_mesh(
    parent,
    name,
    mesh,
    position,
    rotation=(0.0, 0.0, 0.0),
    scale=(1.0, 1.0, 1.0),
    mat=None,
    extra=None,
):
    node = make_node(name, position, rotation, scale)
    node["components"]["MeshSource"] = {"path": mesh}
    if mat is not None:
        node["components"]["Material"] = mat
    if extra:
        node["components"].update(extra)
    return attach(parent, node)


def add_box_collider(parent, name, position, half_extents, rotation=(0.0, 0.0, 0.0), friction=0.85):
    node = make_node(name, position, rotation)
    node["components"]["RigidBody"] = {"motion_type": "static", "friction": friction}
    node["components"]["BoxCollider"] = {
        "half_extents": [round(v, 3) for v in half_extents]
    }
    return attach(parent, node)


def sphere_scale(dx, dy=None, dz=None):
    dy = dx if dy is None else dy
    dz = dx if dz is None else dz
    return (dx / 1.5, dy / 1.5, dz / 1.5)


def cyl_scale(diameter, height):
    return (diameter, height, diameter)


def orient_y(direction):
    d = np.asarray(direction, dtype=np.float64)
    length = np.linalg.norm(d)
    if length < 1e-9:
        return (0.0, 0.0, 0.0)
    d /= length
    rx = math.acos(clamp(float(d[1]), -1.0, 1.0))
    ry = math.atan2(float(d[0]), float(d[2]))
    return (rx, ry, 0.0)


def segment(
    parent,
    name,
    start,
    end,
    radius,
    mesh,
    mat,
    scale_xy=1.0,
):
    start = np.asarray(start, dtype=np.float64)
    end = np.asarray(end, dtype=np.float64)
    d = end - start
    length = float(np.linalg.norm(d))
    if length < 1e-6:
        return None
    mid = (start + end) * 0.5
    rotation = orient_y(d)
    scale = (radius * 2.0 * scale_xy, length, radius * 2.0)
    return add_mesh(
        parent, name, mesh, tuple(mid), rotation, scale, mat
    )


# --------------------------------------------------------------------------
# vegetation
# --------------------------------------------------------------------------

LEAF = "jungle_leaf_grain"
LEAF_LIGHT = "jungle_leaf_grain_light"
FERN = "jungle_leaf_grain_light"
BARK = "jungle_bark"
BARK_PALE = "jungle_bark_pale"
ROCK = "jungle_rock"
GROUND = "jungle_ground"
PATH = "jungle_path_tex"
WATER = "jungle_water"

TRUNK_MESH = "assets/jungle_trunk.glb"
CANOPY_MESH = "assets/jungle_canopy.glb"
CANOPY_SPARSE_MESH = "assets/jungle_canopy_sparse.glb"
CANOPY_BROAD_MESH = "assets/jungle_canopy_broad.glb"
CANOPY_FINE_MESH = "assets/jungle_canopy_fine.glb"
LEAF_TUFT_MESH = "assets/jungle_leaf_tuft.glb"
FERN_MESH = "assets/jungle_fern.glb"
BROADLEAF_MESH = "assets/jungle_broadleaf.glb"
GRASS_MESH = "assets/jungle_grass.glb"
PALM_FROND_MESH = "assets/jungle_palm_frond.glb"
AVATAR_MESH = "assets/jungle_avatar.glb"
WING_ATLAS = "jungle_insect_wings"
PALM_TRUNK_MESH = "assets/jungle_palm_trunk.glb"
BANANA_CROWN_MESH = "assets/jungle_banana_crown.glb"
PALM_BARK = "jungle_palm_bark"
BARK_SMOOTH = "jungle_bark_smooth"
AVATAR_ATLAS = "jungle_avatar_atlas"

LEAF_TINTS = [
    (1.0, 1.0, 1.0),
    (0.82, 0.92, 0.78),
    (0.68, 0.82, 0.64),
    (0.92, 1.0, 0.86),
    (0.58, 0.72, 0.54),
    (1.0, 0.92, 0.74),
    (0.48, 0.62, 0.44),
    (0.72, 0.86, 0.66),
    (0.55, 0.68, 0.50),
]
FERN_TINTS = [
    (1.0, 1.0, 1.0),
    (0.78, 0.95, 0.70),
    (0.60, 0.80, 0.54),
    (0.92, 1.0, 0.82),
]
BARK_TINTS = [(1.0, 1.0, 1.0), (0.90, 0.84, 0.76), (0.78, 0.70, 0.62)]


def leaf_material(tint=None, roughness=0.82, light=False):
    return material(
        LEAF_LIGHT if light else LEAF,
        roughness,
        tint if tint is not None else LEAF_TINTS[0],
    )


def plant_ok(x, z, path_clear=2.2, stream_clear=2.6, player_clear=2.4):
    if abs(x) > 44.0 or abs(z) > 44.0:
        return False
    if abs(x - path_center(z)) < path_clear:
        return False
    if abs(z - stream_center(x)) < stream_clear:
        return False
    if math.hypot(x - SPAWN[0], z - SPAWN[2]) < player_clear:
        return False
    return True


SPAWN = (path_center(8.0), 0.0, 8.0)


def tree(
    parent,
    name,
    x,
    z,
    height,
    radius,
    canopy_width,
    rng,
    hero=False,
    lean=0.0,
    yaw=None,
    canopy_mesh=None,
    emergent=False,
    bark_tex=BARK,
):
    ground = terrain_height(x, z)
    if canopy_mesh is None:
        roll = rng.random()
        if emergent or (hero and roll < 0.45):
            canopy_mesh = CANOPY_BROAD_MESH
        elif hero:
            canopy_mesh = CANOPY_MESH
        elif roll < 0.35:
            canopy_mesh = CANOPY_BROAD_MESH
        elif roll < 0.7:
            canopy_mesh = CANOPY_FINE_MESH
        else:
            canopy_mesh = CANOPY_SPARSE_MESH
    root = attach(
        parent,
        make_node(
            name,
            (x, ground, z),
            (0.0, yaw if yaw is not None else rng.uniform(0, TAU), 0.0),
        ),
    )
    trunk_rotation = (lean, 0.0, lean * 0.4)
    # The trunk mesh has its base at y=0 and includes buttress fins.
    add_mesh(
        root,
        name + "_trunk",
        TRUNK_MESH,
        (0.0, 0.0, 0.0),
        trunk_rotation,
        (radius * 2.0, height, radius * 2.0),
        material(bark_tex, 0.95, BARK_TINTS[int(rng.integers(0, 3))]),
    )
    # Box collider centred halfway up the trunk, not scaled by the mesh.
    add_box_collider(
        root,
        name + "_trunk_collider",
        (0.0, height * 0.5, 0.0),
        (radius, height * 0.5, radius),
        trunk_rotation,
        friction=0.8,
    )
    if hero:
        for i in range(2):
            angle = rng.uniform(0, TAU)
            reach = radius * rng.uniform(4.0, 6.0)
            segment(
                root,
                f"{name}_surface_root{i}",
                (math.cos(angle) * radius * 0.6, 0.16, math.sin(angle) * radius * 0.6),
                (math.cos(angle) * reach, -0.02, math.sin(angle) * reach),
                radius * rng.uniform(0.16, 0.26),
                "builtin:cylinder",
                material(bark_tex, 0.95, BARK_TINTS[int(rng.integers(0, 3))]),
            )
        # Epiphytes on the hero trunks.
        for i in range(2):
            y = rng.uniform(2.0, min(5.5, height * 0.55))
            angle = rng.uniform(0, TAU)
            add_mesh(
                root,
                f"{name}_epiphyte{i}",
                CANOPY_SPARSE_MESH,
                (
                    math.cos(angle) * radius * 1.05,
                    y,
                    math.sin(angle) * radius * 1.05,
                ),
                (rng.uniform(-0.3, 0.3), rng.uniform(0, TAU), 0.0),
                (0.48, 0.34, 0.48),
                leaf_material(LEAF_TINTS[int(rng.integers(1, 5))], 0.85, light=True),
            )
        # Epiphytic ferns clinging to the trunk.
        for i in range(2):
            y = rng.uniform(1.6, min(6.0, height * 0.5))
            angle = rng.uniform(0, TAU)
            add_mesh(
                root,
                f"{name}_epiphyte_fern{i}",
                FERN_MESH,
                (
                    math.cos(angle) * radius * 0.9,
                    y,
                    math.sin(angle) * radius * 0.9,
                ),
                (rng.uniform(-0.4, 0.4), rng.uniform(0, TAU), 0.0),
                (0.55, 0.45, 0.55),
                material(LEAF, 0.88, (0.38, 0.54, 0.34)),
            )

    canopy_base = height * rng.uniform(0.55, 0.78)
    blob_count = 10 if hero else 5
    blob_positions = []
    for i in range(blob_count):
        angle = rng.uniform(0, TAU)
        radial = canopy_width * rng.uniform(0.05, 0.62)
        bx = math.cos(angle) * radial
        bz = math.sin(angle) * radial
        by = canopy_base + rng.uniform(-height * 0.11, height * 0.14) + (i == 0) * 1.4
        diameter = canopy_width * rng.uniform(0.80, 1.45)
        tint = LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))]
        world_x = x + bx
        world_z = z + bz
        if (
            abs(world_x - path_center(world_z)) < 4.4
            and -1.0 < world_z < 16.0
        ) or (
            abs(world_x - path_center(world_z)) < 3.2
            and -16.0 < world_z < -7.0
        ):
            continue
        add_mesh(
            root,
            f"{name}_canopy{i}",
            canopy_mesh if (i % 3 != 0) else CANOPY_MESH,
            (bx, by, bz),
            (rng.uniform(-0.12, 0.12), rng.uniform(0, TAU), rng.uniform(-0.12, 0.12)),
            (diameter * 0.5, diameter * 0.5 * rng.uniform(0.62, 0.82), diameter * 0.5),
            leaf_material(tint, 0.85, light=(i % 3 == 0)),
        )
        blob_positions.append((bx, by, bz))
    # Mid-story foliage breaks up the bare trunk at eye level.
    mid_count = 4 if hero else 2
    for i in range(mid_count):
        angle = rng.uniform(0, TAU)
        radial = rng.uniform(0.9, 2.3)
        diameter = rng.uniform(1.4, 3.0)
        midstory_x = x + math.cos(angle) * radial
        midstory_z = z + math.sin(angle) * radial
        if (
            abs(midstory_x - path_center(midstory_z)) < 3.2
            and -1.0 < midstory_z < 16.0
        ) or (
            abs(midstory_x - path_center(midstory_z)) < 2.4
            and -16.0 < midstory_z < -7.0
        ):
            continue
        add_mesh(
            root,
            f"{name}_midstory{i}",
            (CANOPY_SPARSE_MESH if i % 2 else CANOPY_BROAD_MESH),
            (
                math.cos(angle) * radial,
                height * rng.uniform(0.24, 0.62),
                math.sin(angle) * radial,
            ),
            (rng.uniform(-0.2, 0.2), rng.uniform(0, TAU), rng.uniform(-0.2, 0.2)),
            (diameter * 0.5, diameter * 0.35, diameter * 0.5),
            leaf_material(
                LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))], 0.86, light=(i % 2 == 0)
            ),
        )
    # Branches reach into the lower canopy blobs.
    branch_count = min(5 if hero else 3, len(blob_positions))
    for i in range(branch_count):
        target = blob_positions[i]
        start = (0.0, height * rng.uniform(0.55, 0.7), 0.0)
        segment(
            root,
            f"{name}_branch{i}",
            start,
            target,
            radius * rng.uniform(0.22, 0.38),
            "builtin:cylinder",
            material(bark_tex, 0.95, BARK_TINTS[int(rng.integers(0, 3))]),
        )
    # Hanging lianas.
    vine_count = 3 if hero else 0
    for i in range(vine_count):
        start = (
            rng.uniform(-1.2, 1.2),
            canopy_base - rng.uniform(0.2, 1.4),
            rng.uniform(-1.2, 1.2),
        )
        length = rng.uniform(3.5, 6.5)
        end = (
            start[0] + rng.uniform(-0.7, 0.7),
            max(0.4, start[1] - length),
            start[2] + rng.uniform(-0.7, 0.7),
        )
        segment(
            root,
            f"{name}_liana{i}",
            start,
            end,
            rng.uniform(0.025, 0.045),
            "builtin:cylinder",
            material(bark_tex, 0.9, (0.62, 0.72, 0.46)),
        )
        for j in range(2):
            t = rng.uniform(0.25, 0.85)
            px = lerp(start[0], end[0], t)
            py = lerp(start[1], end[1], t)
            pz = lerp(start[2], end[2], t)
            add_mesh(
                root,
                f"{name}_liana{i}_leaves{j}",
                LEAF_TUFT_MESH,
                (px, py, pz),
                (0, rng.uniform(0, TAU), 0),
                (rng.uniform(0.35, 0.7), rng.uniform(0.25, 0.5), rng.uniform(0.35, 0.7)),
                leaf_material(LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))], 0.86),
            )
    return root


def palm(parent, name, x, z, height, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    lean = rng.uniform(-0.08, 0.08)
    add_mesh(
        root,
        name + "_trunk",
        PALM_TRUNK_MESH,
        (0.0, 0.0, 0.0),
        (lean * 0.7, 0.0, lean),
        (0.34, height, 0.34),
        material(PALM_BARK, 0.88, (0.95, 0.95, 0.86)),
    )
    add_box_collider(
        root,
        name + "_trunk_collider",
        (0.0, height * 0.5, 0.0),
        (0.16, height * 0.5, 0.16),
        friction=0.8,
    )
    crown = (lean * height * 0.6, height, 0.0)
    add_mesh(
        root,
        name + "_crown",
        "builtin:sphere:16:12",
        crown,
        (0, 0, 0),
        sphere_scale(0.55, 0.42, 0.55),
        material(BARK_PALE, 0.9, (0.9, 0.9, 0.74)),
    )
    for i in range(2):
        angle = rng.uniform(0, TAU)
        add_mesh(
            root,
            f"{name}_crown_leaves{i}",
            LEAF_TUFT_MESH,
            (
                crown[0] + math.cos(angle) * rng.uniform(0.4, 0.9),
                crown[1] - rng.uniform(0.2, 0.8),
                crown[2] + math.sin(angle) * rng.uniform(0.4, 0.9),
            ),
            (0, rng.uniform(0, TAU), 0),
            (rng.uniform(0.9, 1.4), rng.uniform(0.5, 0.8), rng.uniform(0.9, 1.4)),
            leaf_material(LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))], 0.84),
        )
    frond_count = int(rng.integers(14, 19))
    for i in range(frond_count):
        angle = TAU * i / frond_count + rng.uniform(-0.12, 0.12)
        scale = rng.uniform(4.2, 6.2)
        add_mesh(
            root,
            f"{name}_frond{i}",
            PALM_FROND_MESH,
            crown,
            (rng.uniform(-0.08, 0.08), angle, rng.uniform(-0.06, 0.06)),
            (scale, scale * rng.uniform(0.8, 1.0), scale),
            leaf_material(FERN_TINTS[int(rng.integers(0, 4))], 0.8, light=(i % 3 == 0)),
        )
    # A skirt of dead, brown fronds under the live crown.
    for i in range(4):
        angle = TAU * i / 4 + rng.uniform(-0.2, 0.2)
        scale = rng.uniform(3.4, 4.8)
        add_mesh(
            root,
            f"{name}_dead_frond{i}",
            PALM_FROND_MESH,
            (crown[0], crown[1] - rng.uniform(0.35, 0.8), crown[2]),
            (rng.uniform(0.12, 0.3), angle, rng.uniform(-0.12, 0.12)),
            (scale, scale * 0.8, scale),
            material(LEAF, 0.9, (0.34, 0.28, 0.14)),
        )
    for i in range(3):
        add_mesh(
            root,
            f"{name}_coconut{i}",
            "builtin:sphere:16:12",
            (
                crown[0] + rng.uniform(-0.3, 0.3),
                crown[1] - rng.uniform(0.25, 0.5),
                crown[2] + rng.uniform(-0.3, 0.3),
            ),
            (0, 0, 0),
            sphere_scale(0.16),
            material(CANVAS_TEX, 0.75, (0.42, 0.34, 0.20)),
        )
    return root


def banana(parent, name, x, z, height, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    stem_mat = material(LEAF_LIGHT, 0.9, (0.52, 0.66, 0.36))
    add_mesh(
        root,
        name + "_stem",
        "builtin:cylinder",
        (0.0, height * 0.5, 0.0),
        (rng.uniform(-0.05, 0.05), 0.0, rng.uniform(-0.05, 0.05)),
        (height * 0.14, height, height * 0.14),
        stem_mat,
    )
    add_mesh(
        root,
        name + "_crown_base",
        "builtin:sphere:16:12",
        (0.0, height - 0.02, 0.0),
        (0.0, 0.0, 0.0),
        sphere_scale(height * 0.20, height * 0.12, height * 0.20),
        material(LEAF_LIGHT, 0.88, (0.46, 0.60, 0.32)),
    )
    # Leaf-base sheaths around the pseudostem.
    for i in range(3):
        add_mesh(
            root,
            f"{name}_sheath{i}",
            "builtin:cone",
            (rng.uniform(-0.08, 0.08), rng.uniform(0.15, 0.55),
             rng.uniform(-0.08, 0.08)),
            (rng.uniform(-0.1, 0.1), rng.uniform(0, TAU), rng.uniform(-0.1, 0.1)),
            (height * rng.uniform(0.20, 0.32), height * rng.uniform(0.25, 0.45),
             height * rng.uniform(0.20, 0.32)),
            material(LEAF, 0.9, (0.38, 0.52, 0.28)),
        )
    # Broad paddle leaves in two offset crowns.
    layers = 2
    for layer in range(layers):
        scale = height * rng.uniform(0.40, 0.55) * (1.0 - layer * 0.14)
        add_mesh(
            root,
            f"{name}_crown{layer}",
            BANANA_CROWN_MESH,
            (rng.uniform(-0.08, 0.08), height - layer * 0.08,
             rng.uniform(-0.08, 0.08)),
            (rng.uniform(-0.05, 0.05), rng.uniform(0, TAU),
             rng.uniform(-0.05, 0.05)),
            (scale, scale * rng.uniform(0.8, 1.0), scale),
            material(LEAF_LIGHT, 0.84, LEAF_TINTS[int(rng.integers(0, 5))]),
        )
    # A small green/yellow banana bunch under the crown.
    bunch_z = height - 0.45
    for i in range(6):
        angle = TAU * i / 6
        add_mesh(
            root,
            f"{name}_banana{i}",
            "builtin:cylinder",
            (math.cos(angle) * 0.10, bunch_z + rng.uniform(-0.04, 0.04),
             math.sin(angle) * 0.10),
            (PI / 2.0 + rng.uniform(-0.3, 0.3), angle, 0.0),
            (0.035, rng.uniform(0.13, 0.20), 0.035),
            material(CANVAS_TEX, 0.75, (0.68, 0.62, 0.22)),
        )
    add_mesh(
        root,
        name + "_flower",
        "builtin:cone",
        (0.0, bunch_z - 0.15, 0.0),
        (PI, 0.0, 0.0),
        (0.10, 0.25, 0.10),
        material(None, 0.85, (0.42, 0.10, 0.16)),
    )
    return root


def tree_fern(parent, name, x, z, height, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    add_mesh(
        root,
        name + "_trunk",
        "builtin:cylinder",
        (0.0, height * 0.5, 0.0),
        (rng.uniform(-0.04, 0.04), 0.0, rng.uniform(-0.04, 0.04)),
        (height * 0.055, height, height * 0.055),
        material(BARK_PALE, 0.92, (0.62, 0.56, 0.44)),
    )
    add_mesh(
        root,
        name + "_crown0",
        FERN_MESH,
        (0.0, height - 0.05, 0.0),
        (0.0, rng.uniform(0, TAU), 0.0),
        (rng.uniform(1.6, 2.4), rng.uniform(1.4, 2.0), rng.uniform(1.6, 2.4)),
        material(LEAF_LIGHT, 0.86, (0.38, 0.54, 0.32)),
    )
    add_mesh(
        root,
        name + "_crown1",
        FERN_MESH,
        (0.0, height - 0.12, 0.0),
        (0.0, rng.uniform(0, TAU), 0.0),
        (rng.uniform(1.1, 1.7), rng.uniform(1.0, 1.5), rng.uniform(1.1, 1.7)),
        material(LEAF, 0.87, (0.30, 0.45, 0.26)),
    )
    # Old frond skirt.
    for i in range(4):
        add_mesh(
            root,
            f"{name}_skirt{i}",
            FERN_MESH,
            (0.0, height - rng.uniform(0.25, 0.55), 0.0),
            (rng.uniform(0.2, 0.5), rng.uniform(0, TAU), 0.0),
            (rng.uniform(0.7, 1.1), rng.uniform(0.4, 0.7), rng.uniform(0.7, 1.1)),
            material(LEAF, 0.9, (0.34, 0.30, 0.16)),
        )
    return root


def strangler_fig(parent, name, x, z, height, radius, width, rng):
    root = tree(
        parent, name, x, z, height, radius, width, rng,
        hero=True, lean=rng.uniform(-0.03, 0.03),
        canopy_mesh=CANOPY_BROAD_MESH, emergent=True, bark_tex=BARK_SMOOTH,
    )
    # Aerial roots descend from the crown to the floor around the host.
    count = int(rng.integers(10, 16))
    for i in range(count):
        angle = rng.uniform(0, TAU)
        start_radius = rng.uniform(1.4, 3.8)
        end_radius = start_radius + rng.uniform(-1.2, 1.4)
        start_y = height * rng.uniform(0.50, 0.95)
        start = (
            math.cos(angle) * start_radius,
            start_y,
            math.sin(angle) * start_radius,
        )
        end = (
            math.cos(angle) * end_radius + rng.uniform(-0.3, 0.3),
            -0.03,
            math.sin(angle) * end_radius + rng.uniform(-0.3, 0.3),
        )
        segment(
            root,
            f"{name}_aerial_root{i}",
            start,
            end,
            rng.uniform(0.035, 0.085),
            "builtin:cylinder",
            material(PALM_BARK, 0.92,
                     (0.72, 0.70, 0.60) if i % 2 else (0.56, 0.54, 0.46)),
        )
    return root


def fern(parent, name, x, z, size, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    add_mesh(
        root,
        name + "_plant",
        FERN_MESH,
        (0.0, 0.0, 0.0),
        (0.0, rng.uniform(0, TAU), 0.0),
        (size * rng.uniform(0.9, 1.15), size, size * rng.uniform(0.9, 1.15)),
        material(
            LEAF_LIGHT,
            0.86,
            (0.58, 0.72, 0.50) if rng.random() < 0.6 else (0.44, 0.58, 0.38),
        ),
    )
    return root


def shrub(parent, name, x, z, size, rng, light=False):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    blobs = int(rng.integers(3, 5))
    for i in range(blobs):
        angle = rng.uniform(0, TAU)
        radial = size * rng.uniform(0.0, 0.40)
        diameter = size * rng.uniform(0.55, 0.95)
        cx = math.cos(angle) * radial
        cz = math.sin(angle) * radial
        cy = size * rng.uniform(0.30, 0.75)
        if i < 2:
            segment(
                root,
                f"{name}_stem{i}",
                (0.0, 0.0, 0.0),
                (cx, cy, cz),
                0.025,
                "builtin:cylinder",
                material(BARK, 0.9, (0.55, 0.60, 0.42)),
            )
        add_mesh(
            root,
            f"{name}_leaf{i}",
            LEAF_TUFT_MESH,
            (cx, cy, cz),
            (rng.uniform(-0.2, 0.2), rng.uniform(0, TAU), rng.uniform(-0.2, 0.2)),
            (diameter * 0.5, diameter * 0.5 * rng.uniform(0.65, 0.9), diameter * 0.5),
            leaf_material(
                LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))],
                0.86,
                light or rng.random() < 0.3,
            ),
        )
    return root


def broadleaf(parent, name, x, z, size, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0, rng.uniform(0, TAU), 0)))
    add_mesh(
        root,
        name + "_plant",
        BROADLEAF_MESH,
        (0.0, 0.0, 0.0),
        (0.0, rng.uniform(0, TAU), 0.0),
        (size, size * rng.uniform(0.85, 1.15), size),
        leaf_material(
            LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))], 0.82, light=True
        ),
    )
    return root


def bamboo(parent, name, x, z, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    stalks = int(rng.integers(9, 15))
    for i in range(stalks):
        height = rng.uniform(3.2, 6.4)
        ox = rng.uniform(-0.7, 0.7)
        oz = rng.uniform(-0.7, 0.7)
        lean = rng.uniform(-0.06, 0.06)
        add_mesh(
            root,
            f"{name}_stalk{i}",
            "builtin:cylinder",
            (ox, height * 0.5, oz),
            (lean, 0.0, lean * 1.5),
            cyl_scale(rng.uniform(0.055, 0.085), height),
            material(BARK_PALE, 0.8, (0.62, 0.78, 0.42)),
        )
        for j in range(2):
            add_mesh(
                root,
                f"{name}_leaf{i}_{j}",
                LEAF_TUFT_MESH,
                (
                    ox + rng.uniform(-0.35, 0.35),
                    height - rng.uniform(0.0, 0.7),
                    oz + rng.uniform(-0.35, 0.35),
                ),
                (rng.uniform(-0.2, 0.2), rng.uniform(0, TAU), 0.0),
                (rng.uniform(0.28, 0.45), rng.uniform(0.18, 0.28), rng.uniform(0.28, 0.45)),
                leaf_material(LEAF_TINTS[int(rng.integers(0, len(LEAF_TINTS)))], 0.82),
            )
    return root


def grass_tuft(parent, name, x, z, size, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0, rng.uniform(0, TAU), 0)))
    add_mesh(
        root,
        name + "_grass",
        GRASS_MESH,
        (0.0, 0.0, 0.0),
        (0.0, rng.uniform(0, TAU), 0.0),
        (size * rng.uniform(0.8, 1.2), size, size * rng.uniform(0.8, 1.2)),
        material(FERN, 0.9, FERN_TINTS[int(rng.integers(0, 4))]),
    )
    return root


def ground_cover(parent, name, x, z, size, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    kind = rng.random()
    if kind < 0.45:
        add_mesh(
            root,
            name + "_fern",
            FERN_MESH,
            (0.0, 0.0, 0.0),
            (0.0, rng.uniform(0, TAU), 0.0),
            (size * rng.uniform(0.4, 0.7), size * rng.uniform(0.35, 0.6), size * rng.uniform(0.4, 0.7)),
            material(LEAF, 0.88, (0.36, 0.50, 0.32)),
        )
    elif kind < 0.8:
        add_mesh(
            root,
            name + "_broadleaf",
            BROADLEAF_MESH,
            (0.0, 0.0, 0.0),
            (0.0, rng.uniform(0, TAU), 0.0),
            (size * rng.uniform(0.4, 0.75), size * rng.uniform(0.35, 0.65), size * rng.uniform(0.4, 0.75)),
            material(LEAF, 0.86, (0.34, 0.48, 0.31)),
        )
    else:
        add_mesh(
            root,
            name + "_grass",
            GRASS_MESH,
            (0.0, 0.0, 0.0),
            (0.0, rng.uniform(0, TAU), 0.0),
            (size * rng.uniform(0.4, 0.8), size * rng.uniform(0.4, 0.8), size * rng.uniform(0.4, 0.8)),
            material(LEAF, 0.9, (0.32, 0.46, 0.30)),
        )
    return root


def moss_patch(parent, name, x, z, size, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z), (0.0, rng.uniform(0, TAU), 0.0)))
    for i in range(int(rng.integers(2, 5))):
        angle = rng.uniform(0, TAU)
        radial = size * rng.uniform(0.0, 0.45)
        diameter = size * rng.uniform(0.5, 1.15)
        add_mesh(
            root,
            f"{name}_moss{i}",
            LEAF_TUFT_MESH,
            (
                math.cos(angle) * radial,
                rng.uniform(0.03, 0.13),
                math.sin(angle) * radial,
            ),
            (rng.uniform(-0.15, 0.15), rng.uniform(0, TAU), rng.uniform(-0.15, 0.15)),
            (diameter * 0.5, diameter * rng.uniform(0.14, 0.28), diameter * 0.5),
            material(LEAF, 0.92, (0.24, 0.38, 0.18)),
        )
    return root


def flower_cluster(parent, name, x, z, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    palettes = [
        (0.82, 0.16, 0.12),
        (0.92, 0.44, 0.12),
        (0.86, 0.22, 0.48),
        (0.92, 0.84, 0.66),
        (0.55, 0.18, 0.68),
    ]
    bloom = palettes[int(rng.integers(0, len(palettes)))]
    stems = int(rng.integers(3, 6))
    for i in range(stems):
        h = rng.uniform(0.45, 1.15)
        ox = rng.uniform(-0.35, 0.35)
        oz = rng.uniform(-0.35, 0.35)
        add_mesh(
            root,
            f"{name}_stem{i}",
            "builtin:cylinder",
            (ox, h * 0.5, oz),
            (rng.uniform(-0.08, 0.08), 0, rng.uniform(-0.08, 0.08)),
            (0.022, h, 0.022),
            material(FERN, 0.9, (0.40, 0.55, 0.22)),
        )
        add_mesh(
            root,
            f"{name}_bloom{i}",
            "builtin:sphere:16:12",
            (ox, h, oz),
            (0, rng.uniform(0, TAU), 0),
            sphere_scale(rng.uniform(0.10, 0.17), rng.uniform(0.06, 0.10), rng.uniform(0.10, 0.17)),
            material(CANVAS_TEX, 0.7, bloom),
        )
    return root


def rock(parent, name, x, z, size, rng, collider=False):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    sx = size * rng.uniform(0.7, 1.25)
    sy = size * rng.uniform(0.45, 0.8)
    sz = size * rng.uniform(0.7, 1.2)
    extra = None
    if collider and size > 0.9:
        extra = {
            "RigidBody": {"motion_type": "static", "friction": 0.8},
            "SphereCollider": {"radius": round(max(sx, sz) * 0.45, 3)},
        }
    add_mesh(
        root,
        name + "_rock",
        "builtin:sphere:24:16",
        (0.0, sy * 0.22, 0.0),
        (rng.uniform(-0.25, 0.25), rng.uniform(0, TAU), rng.uniform(-0.25, 0.25)),
        sphere_scale(sx, sy, sz),
        material(ROCK, 0.92, (0.95, 0.96, 0.92)),
        extra,
    )
    if rng.random() < 0.7:
        add_mesh(
            root,
            name + "_moss",
            "builtin:sphere:16:12",
            (rng.uniform(-0.15, 0.15) * size, sy * 0.62, rng.uniform(-0.15, 0.15) * size),
            (0, rng.uniform(0, TAU), 0),
            sphere_scale(sx * 0.8, sy * 0.28, sz * 0.8),
            material(LEAF, 0.9, (0.42, 0.62, 0.28)),
        )
    return root


def fallen_log(parent, name, x, z, length, radius, angle, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    direction = (math.cos(angle), 0.0, math.sin(angle))
    mid = (direction[0] * length * 0.5, radius * 0.9, direction[2] * length * 0.5)
    add_mesh(
        root,
        name + "_log",
        "builtin:cylinder",
        mid,
        orient_y(direction),
        cyl_scale(radius * 2.0, length),
        material(BARK, 0.95, (0.88, 0.90, 0.82)),
        {
            "RigidBody": {"motion_type": "static", "friction": 0.9},
            "BoxCollider": {
                "half_extents": [round(radius, 3), round(length * 0.5, 3), round(radius, 3)]
            },
        },
    )
    for i in range(4):
        t = rng.uniform(0.1, 0.9)
        add_mesh(
            root,
            f"{name}_moss{i}",
            "builtin:sphere:16:12",
            (
                direction[0] * length * t + rng.uniform(-0.2, 0.2),
                radius * rng.uniform(1.2, 1.7),
                direction[2] * length * t + rng.uniform(-0.2, 0.2),
            ),
            (0, rng.uniform(0, TAU), 0),
            sphere_scale(rng.uniform(0.45, 0.9), rng.uniform(0.1, 0.2), rng.uniform(0.45, 0.9)),
            material(LEAF, 0.9, (0.40, 0.58, 0.24)),
        )
    return root


def stump(parent, name, x, z, radius, height, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    add_mesh(
        root,
        name + "_stem",
        "builtin:cylinder",
        (0.0, height * 0.5, 0.0),
        (0, 0, 0),
        cyl_scale(radius * 2.0, height),
        material(BARK, 0.95, (0.8, 0.8, 0.74)),
        {
            "RigidBody": {"motion_type": "static", "friction": 0.9},
            "BoxCollider": {"half_extents": [round(radius, 3), round(height * 0.5, 3), round(radius, 3)]},
        },
    )
    add_mesh(
        root,
        name + "_top",
        "builtin:cylinder",
        (0.0, height + 0.01, 0.0),
        (0, 0, 0),
        (radius * 2.0, 0.04, radius * 2.0),
        material(BARK_PALE, 0.9, (0.78, 0.64, 0.42)),
    )
    return root


CANVAS_TEX = "jungle_canvas"


# --------------------------------------------------------------------------
# bridge
# --------------------------------------------------------------------------


def add_wing(parent, name, species, side, position, scale=1.0):
    tints = {
        "dragonfly": (0.62, 0.72, 0.76),
        "monarch": (0.92, 0.72, 0.48),
        "morpho": (0.58, 0.70, 1.0),
        "bee": (0.74, 0.66, 0.46),
    }
    return add_mesh(
        parent,
        name,
        WING_MESHES[(species, side)],
        position,
        (0.0, 0.0, 0.0),
        (scale, scale, scale),
        material(WING_ATLAS, 0.32, tints.get(species, (1.0, 1.0, 1.0))),
    )


def add_insect_part(parent, name, mesh, position, rotation, scale, mat):
    return add_mesh(parent, name, mesh, position, rotation, scale, mat)


def dragonfly(parent, name, x, y, z, rng):
    root = attach(parent, make_node(name, (x, y, z)))
    root["components"]["Script"] = {"path": "scripts/insect_flight.lua"}
    chitin = material(None, 0.34, (0.035, 0.145, 0.135))
    dark = material(None, 0.55, (0.015, 0.035, 0.030))
    eye = material(None, 0.12, (0.020, 0.025, 0.030))
    add_insect_part(root, name + "_abdomen_a", "builtin:cylinder",
                    (0.0, 0.0, -0.055), (PI / 2.0, 0.0, 0.0),
                    (0.020, 0.105, 0.020), chitin)
    add_insect_part(root, name + "_abdomen_b", "builtin:cylinder",
                    (0.0, 0.0, -0.130), (PI / 2.0, 0.0, 0.0),
                    (0.013, 0.075, 0.013), chitin)
    add_insect_part(root, name + "_tail", "builtin:sphere:12:8",
                    (0.0, 0.0, -0.170), (0, 0, 0),
                    sphere_scale(0.010), dark)
    add_insect_part(root, name + "_thorax", "builtin:sphere:16:12",
                    (0.0, 0.0, 0.0), (0, 0, 0),
                    sphere_scale(0.042, 0.036, 0.055), chitin)
    add_insect_part(root, name + "_head", "builtin:sphere:16:12",
                    (0.0, 0.004, 0.050), (0, 0, 0),
                    sphere_scale(0.032, 0.030, 0.030), chitin)
    for side, sx in (("l", -1.0), ("r", 1.0)):
        add_insect_part(root, name + "_eye_" + side, "builtin:sphere:12:8",
                        (sx * 0.014, 0.014, 0.060), (0, 0, 0),
                        sphere_scale(0.022), eye)
        add_wing(root, name + "_wing_" + side + "0", "dragonfly", sx,
                 (sx * 0.010, 0.018, 0.010), 1.0)
        add_wing(root, name + "_wing_" + side + "1", "dragonfly", sx,
                 (sx * 0.010, 0.018, -0.020), 0.92)
        for leg in range(3):
            zz = 0.025 - leg * 0.025
            segment(root, f"{name}_leg_{side}{leg}",
                    (sx * 0.010, -0.010, zz),
                    (sx * 0.030, -0.045, zz + 0.010),
                    0.0018, "builtin:cylinder", dark)
    return root


def butterfly(parent, name, x, y, z, rng, species="monarch"):
    root = attach(parent, make_node(name, (x, y, z), (0.0, rng.uniform(0, TAU), 0.0)))
    root["components"]["Script"] = {"path": "scripts/insect_flight.lua"}
    body = material(None, 0.72, (0.045, 0.032, 0.020))
    dark = material(None, 0.55, (0.020, 0.015, 0.010))
    add_insect_part(root, name + "_abdomen", "builtin:cylinder",
                    (0.0, 0.0, -0.030), (PI / 2.0, 0.0, 0.0),
                    (0.013, 0.065, 0.013), body)
    add_insect_part(root, name + "_thorax", "builtin:sphere:16:12",
                    (0.0, 0.0, 0.010), (0, 0, 0),
                    sphere_scale(0.024, 0.022, 0.030), body)
    add_insect_part(root, name + "_head", "builtin:sphere:12:8",
                    (0.0, 0.0, 0.040), (0, 0, 0),
                    sphere_scale(0.018), dark)
    for side, sx in (("l", -1.0), ("r", 1.0)):
        add_wing(root, name + "_wing_" + side + "0", species, sx,
                 (sx * 0.006, 0.012, 0.010), 1.0)
        add_wing(root, name + "_wing_" + side + "1", species, sx,
                 (sx * 0.006, 0.008, -0.020), 0.68)
        segment(root, f"{name}_antenna_{side}",
                (sx * 0.004, 0.006, 0.050),
                (sx * 0.035, 0.045, 0.080),
                0.0015, "builtin:cylinder", dark)
    return root


def bee(parent, name, x, y, z, rng):
    root = attach(parent, make_node(name, (x, y, z), (0.0, rng.uniform(0, TAU), 0.0)))
    root["components"]["Script"] = {"path": "scripts/insect_flight.lua"}
    yellow = material(None, 0.62, (0.72, 0.48, 0.06))
    dark = material(None, 0.55, (0.025, 0.022, 0.015))
    eye = material(None, 0.15, (0.015, 0.018, 0.015))
    add_insect_part(root, name + "_abdomen", "builtin:sphere:16:12",
                    (0.0, 0.0, -0.018), (0, 0, 0),
                    sphere_scale(0.032, 0.028, 0.048), yellow)
    for i, zz in enumerate((-0.010, -0.030)):
        add_insect_part(root, f"{name}_stripe{i}", "builtin:cylinder",
                        (0.0, 0.0, zz), (PI / 2.0, 0.0, 0.0),
                        (0.033, 0.007, 0.033), dark)
    add_insect_part(root, name + "_thorax", "builtin:sphere:16:12",
                    (0.0, 0.0, 0.020), (0, 0, 0),
                    sphere_scale(0.026, 0.024, 0.028), dark)
    add_insect_part(root, name + "_head", "builtin:sphere:12:8",
                    (0.0, 0.0, 0.045), (0, 0, 0),
                    sphere_scale(0.018), dark)
    for side, sx in (("l", -1.0), ("r", 1.0)):
        add_insect_part(root, name + "_eye_" + side, "builtin:sphere:12:8",
                        (sx * 0.010, 0.004, 0.050), (0, 0, 0),
                        sphere_scale(0.012), eye)
        add_wing(root, name + "_wing_" + side + "0", "bee", sx,
                 (sx * 0.006, 0.014, 0.015), 1.0)
        add_wing(root, name + "_wing_" + side + "1", "bee", sx,
                 (sx * 0.006, 0.010, 0.000), 0.75)
        segment(root, f"{name}_antenna_{side}",
                (sx * 0.004, 0.006, 0.055),
                (sx * 0.022, 0.024, 0.072),
                0.0012, "builtin:cylinder", dark)
    return root


def build_insects(parent, rng):
    dragonflies = [
        (-2.0, -11.0), (0.9, 4.5), (2.4, 0.6), (-1.1, -6.2),
        (4.1, -2.0), (-4.2, -16.0), (1.8, 6.2), (-3.0, 2.5),
        (0.5, 6.0), (-1.5, 7.5), (1.5, 10.0), (-1.4, 9.4),
    ]
    for i, (x, z) in enumerate(dragonflies):
        dragonfly(
            parent, f"dragonfly_{i:02d}", x,
            terrain_height(x, z) + rng.uniform(1.05, 1.75), z, rng,
        )
    butterflies = [
        (2.4, 1.0, 1.55), (-2.2, 4.2, 1.7), (-4.6, -3.5, 1.45),
        (3.4, -8.5, 1.8), (-1.6, -16.0, 1.5), (5.8, 2.4, 1.9),
        (-6.0, -18.0, 1.6), (2.0, 15.0, 1.7), (8.5, -3.0, 1.5),
        (-2.9, 7.8, 1.35), (1.4, 6.0, 1.65), (-1.8, 5.2, 1.5),
        (2.0, 10.0, 1.8), (2.0, 9.0, 1.9), (-2.0, 9.0, 1.75),
    ]
    for i, (x, z, altitude) in enumerate(butterflies):
        species = "morpho" if i % 3 == 0 else "monarch"
        butterfly(
            parent, f"butterfly_{i:02d}", x,
            terrain_height(x, z) + altitude, z, rng, species,
        )
    bees = [
        (-2.2, 4.2), (2.4, 1.0), (-4.6, -3.5), (3.4, -8.5),
        (2.0, 15.0), (-2.9, 7.8),
    ]
    for i, (x, z) in enumerate(bees):
        bee(
            parent, f"bee_{i:02d}", x,
            terrain_height(x, z) + rng.uniform(0.9, 1.7), z, rng,
        )


def campfire(parent, name, x, z, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    rock_mat = material(ROCK, 0.92, (0.9, 0.9, 0.86))
    wood_mat = material(BARK, 0.92, (0.62, 0.48, 0.32))
    for i in range(9):
        angle = TAU * i / 9 + rng.uniform(-0.1, 0.1)
        radius = rng.uniform(0.55, 0.8)
        size = rng.uniform(0.18, 0.32)
        add_mesh(
            root,
            f"{name}_stone{i}",
            "builtin:sphere:16:12",
            (math.cos(angle) * radius, size * 0.35, math.sin(angle) * radius),
            (rng.uniform(-0.2, 0.2), rng.uniform(0, TAU), rng.uniform(-0.2, 0.2)),
            sphere_scale(size, size * 0.6, size),
            rock_mat,
        )
    for i in range(4):
        angle = TAU * i / 4 + 0.3
        start = (math.cos(angle) * 0.55, 0.11, math.sin(angle) * 0.55)
        end = (-math.cos(angle) * 0.55, 0.11, -math.sin(angle) * 0.55)
        segment(root, f"{name}_log{i}", start, end, 0.09,
                "builtin:cylinder", wood_mat)
    flame_mats = [
        material(None, 0.35, (1.0, 0.42, 0.08),
                 emissive=(1.0, 0.30, 0.03)),
        material(None, 0.30, (1.0, 0.65, 0.12),
                 emissive=(1.0, 0.55, 0.08)),
        material(None, 0.25, (1.0, 0.90, 0.45),
                 emissive=(1.0, 0.80, 0.30)),
    ]
    for i in range(5):
        angle = TAU * i / 5
        height = rng.uniform(0.28, 0.55)
        add_mesh(
            root,
            f"{name}_flame{i}",
            "builtin:cone",
            (math.cos(angle) * 0.10, 0.22 + height * 0.5,
             math.sin(angle) * 0.10),
            (rng.uniform(-0.15, 0.15), angle, rng.uniform(-0.15, 0.15)),
            (rng.uniform(0.12, 0.22), height, rng.uniform(0.10, 0.18)),
            flame_mats[i % len(flame_mats)],
        )
    add_mesh(
        root,
        name + "_ember_core",
        "builtin:sphere:16:12",
        (0.0, 0.24, 0.0),
        (0, 0, 0),
        sphere_scale(0.22, 0.16, 0.22),
        flame_mats[1],
    )
    light = make_node(name + "_light", (0.0, 0.55, 0.0))
    light["components"]["PointLight"] = {
        "color": [1.0, 0.52, 0.18],
        "intensity": 7.0,
        "range": 7.0,
    }
    attach(root, light)
    return root


def lantern(parent, name, x, z, height, rng):
    ground = terrain_height(x, z)
    root = attach(parent, make_node(name, (x, ground, z)))
    metal = material(None, 0.34, (0.20, 0.20, 0.19), metallic=1.0)
    wood = material(BARK, 0.9, (0.55, 0.45, 0.30))
    add_mesh(root, name + "_post", "builtin:cylinder",
             (0.0, height * 0.5, 0.0), (0, 0, 0),
             (0.10, height, 0.10), wood)
    add_mesh(root, name + "_arm", "builtin:cylinder",
             (0.0, height, 0.16), (PI / 2.0, 0.0, 0.0),
             (0.06, 0.36, 0.06), metal)
    add_mesh(root, name + "_cage", "builtin:cube",
             (0.0, height - 0.22, 0.30), (0, 0, 0),
             (0.22, 0.28, 0.20), metal)
    glass = material(None, 0.10, (1.0, 0.92, 0.62),
                     emissive=(1.0, 0.62, 0.18))
    add_mesh(root, name + "_glass", "builtin:sphere:16:12",
             (0.0, height - 0.22, 0.30), (0, 0, 0),
             sphere_scale(0.16, 0.18, 0.14), glass)
    add_mesh(root, name + "_cap", "builtin:cone",
             (0.0, height - 0.05, 0.30), (0, 0, 0),
             (0.27, 0.12, 0.24), metal)
    light = make_node(name + "_light", (0.0, height - 0.22, 0.30))
    light["components"]["PointLight"] = {
        "color": [1.0, 0.72, 0.32],
        "intensity": 3.2,
        "range": 4.5,
    }
    attach(root, light)
    return root


def firefly(parent, name, x, y, z, rng):
    root = attach(parent, make_node(name, (x, y, z)))
    glow = material(None, 0.2, (1.0, 0.95, 0.55),
                    emissive=(1.0, 0.90, 0.35))
    add_mesh(root, name + "_glow", "builtin:sphere:12:8",
             (0, 0, 0), (0, 0, 0), sphere_scale(0.055), glow)
    light = make_node(name + "_light", (0, 0, 0))
    light["components"]["PointLight"] = {
        "color": [0.85, 1.0, 0.48],
        "intensity": 1.4,
        "range": 3.0,
    }
    attach(root, light)
    return root


def build_bridge(parent, rng):
    bx, bz = bridge_center()
    root = attach(parent, make_node("bridge", (bx, 0.0, bz)))
    south_y = terrain_height(path_center(bz + 3.4), bz + 3.4)
    north_y = terrain_height(path_center(bz - 3.4), bz - 3.4)
    deck_y = (south_y + north_y) * 0.5 + 0.04
    deck_z = 3.35
    deck = make_node("bridge_deck_collider", (0.0, deck_y - 0.005, 0.0))
    deck["components"]["RigidBody"] = {"motion_type": "static", "friction": 0.9}
    deck["components"]["BoxCollider"] = {"half_extents": [1.10, 0.05, deck_z]}
    attach(root, deck)
    # Planks ride on top of the collision slab.
    planks = 14
    for i in range(planks):
        z = -deck_z + 0.25 + i * (2.0 * deck_z - 0.5) / (planks - 1)
        add_mesh(
            root,
            f"bridge_plank{i}",
            "builtin:cube",
            (0.0, deck_y, z),
            (0.0, rng.uniform(-0.014, 0.014), 0.0),
            (2.25, 0.09, 0.46),
            material(BARK, 0.86, (1.25, 1.08, 0.82)),
        )
    for side in (-1.0, 1.0):
        add_mesh(
            root,
            "bridge_beam" + ("_w" if side < 0 else "_e"),
            "builtin:cylinder",
            (side * 0.95, deck_y - 0.24, 0.0),
            (PI / 2.0, 0.0, 0.0),
            (0.22, 7.2, 0.22),
            material(BARK, 0.92, (0.82, 0.74, 0.58)),
        )
        # Rail and posts make the crossing read as a footbridge.
        add_mesh(
            root,
            "bridge_rail" + ("_w" if side < 0 else "_e"),
            "builtin:cylinder",
            (side * 1.04, deck_y + 0.68, 0.0),
            (PI / 2.0, 0.0, 0.0),
            (0.10, 7.0, 0.10),
            material(BARK, 0.9, (0.82, 0.72, 0.54)),
        )
        for pz in (-2.95, 0.0, 2.95):
            add_mesh(
                root,
                f"bridge_post_{'w' if side < 0 else 'e'}_{pz:+.1f}".replace(".", "_"),
                "builtin:cylinder",
                (side * 1.04, deck_y + 0.34, pz),
                (0.0, 0.0, 0.0),
                (0.13, 1.22, 0.13),
                material(BARK, 0.9, (0.80, 0.70, 0.52)),
            )
    return root


# --------------------------------------------------------------------------
# character
# --------------------------------------------------------------------------


def build_character(parent):
    ground = terrain_height(SPAWN[0], SPAWN[2])
    player = attach(
        parent,
        make_node("player", (SPAWN[0], ground + 0.06, SPAWN[2]), (0.0, PI + 0.10, 0.0)),
    )
    player["components"]["CharacterController"] = {"radius": 0.30, "half_height": 0.54}
    avatar = add_mesh(
        player,
        "avatar",
        AVATAR_MESH,
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 0.0),
        (1.0, 1.0, 1.0),
        material(AVATAR_ATLAS, 0.80, (1.0, 1.0, 1.0)),
    )
    avatar["components"]["Animator"] = {
        "clip_id": "jungle_idle",
        "speed": 0.9,
        "loop": True,
        "playing": True,
    }
    # A soft analytic contact shadow keeps the feet planted even at noon.
    contact = make_node("player_contact", (0.0, 0.03, 0.0))
    contact["components"]["BlobCaster"] = {"radius": 0.45}
    attach(player, contact)
    player["_avatar_guid"] = avatar["guid"]
    return player


# --------------------------------------------------------------------------
# scene assembly
# --------------------------------------------------------------------------


def build_scene():
    global ROOTS, GUID_COUNTER
    ROOTS = []
    GUID_COUNTER = 0x200
    GUID_INDEX.clear()
    rng = np.random.default_rng(SEED)

    world = attach(None, make_node("world"))
    ground_group = attach(world, make_node("terrain_group"))
    add_mesh(
        ground_group,
        "terrain",
        "assets/jungle_terrain.glb",
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 0.0),
        (1.0, 1.0, 1.0),
        material(GROUND, 0.97, (1.0, 1.0, 1.0)),
        {
            "RigidBody": {"motion_type": "static", "friction": 0.95},
            "HeightFieldCollider": {"min_height": -1.0, "max_height": 6.0},
        },
    )
    add_mesh(
        ground_group,
        "trail",
        "assets/jungle_path.glb",
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 0.0),
        (1.0, 1.0, 1.0),
        material(PATH, 0.98, (1.0, 1.0, 1.0)),
    )
    add_mesh(
        ground_group,
        "stream",
        "assets/jungle_stream.glb",
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 0.0),
        (1.0, 1.0, 1.0),
        material(WATER, 0.14, (0.42, 0.62, 0.58)),
    )
    build_bridge(ground_group, rng)

    veg = attach(world, make_node("vegetation"))

    # Hero trees: these frame the authored camera.
    tree(veg, "tree_west_hero", -3.4, 5.8, 11.5, 0.48, 5.4, rng, hero=True, lean=0.03, yaw=0.7)
    tree(veg, "tree_east_hero", 3.7, 3.6, 12.5, 0.44, 5.8, rng, hero=True, lean=-0.02, yaw=2.2)
    tree(veg, "tree_west_mid", -5.2, -0.8, 10.2, 0.36, 4.4, rng, hero=True, lean=0.05, yaw=4.0)
    tree(veg, "tree_east_mid", 5.0, -4.2, 13.0, 0.52, 6.0, rng, hero=True, lean=-0.04, yaw=1.1)
    tree(veg, "tree_west_far", -6.4, -10.5, 11.0, 0.40, 5.0, rng, hero=True, lean=0.02, yaw=3.3)
    tree(veg, "tree_east_far", 6.8, -16.0, 14.0, 0.55, 6.5, rng, hero=True, lean=-0.03, yaw=5.1)

    # A few large emergent trees behind, plus a dense mid-ground ring.
    tree(veg, "tree_giant_west", -11.5, -6.0, 17.0, 0.8, 8.5, rng, hero=False, lean=0.03, yaw=0.4)
    tree(veg, "tree_giant_east", 12.0, -12.0, 18.0, 0.9, 9.0, rng, hero=False, lean=-0.02, yaw=2.8)
    tree(veg, "tree_giant_south", -10.0, 17.0, 15.0, 0.7, 7.5, rng, hero=False, lean=0.04, yaw=5.7)

    occupied = [(-3.4, 5.8), (3.7, 3.6), (-5.2, -0.8), (5.0, -4.2), (-6.4, -10.5),
                (6.8, -16.0), (-11.5, -6.0), (12.0, -12.0), (-10.0, 17.0)]
    placed = 0
    attempts = 0
    while placed < 40 and attempts < 600:
        attempts += 1
        angle = rng.uniform(0, TAU)
        radius = rng.uniform(13.0, 44.0)
        x = math.cos(angle) * radius
        z = math.sin(angle) * radius - 5.0
        if not plant_ok(x, z, path_clear=3.6, stream_clear=3.8, player_clear=8.0):
            continue
        if any(math.hypot(x - ox, z - oz) < 3.0 for ox, oz in occupied):
            continue
        occupied.append((x, z))
        height = rng.uniform(8.0, 15.0)
        trunk = rng.uniform(0.20, 0.42)
        width = rng.uniform(3.6, 6.5)
        tree(veg, f"tree_{placed:02d}", x, z, height, trunk, width, rng, hero=(placed < 10))
        placed += 1

    # Young understory trees fill the midground between the big trunks.
    saplings = 0
    attempts = 0
    while saplings < 36 and attempts < 700:
        attempts += 1
        x = rng.uniform(-26.0, 26.0)
        z = rng.uniform(-30.0, 22.0)
        if not plant_ok(x, z, path_clear=2.0, stream_clear=3.0, player_clear=7.0):
            continue
        if any(math.hypot(x - ox, z - oz) < 2.2 for ox, oz in occupied):
            continue
        occupied.append((x, z))
        tree(
            veg,
            f"sapling_{saplings:02d}",
            x,
            z,
            rng.uniform(3.0, 6.5),
            rng.uniform(0.08, 0.16),
            rng.uniform(2.2, 4.0),
            rng,
            hero=False,
            lean=rng.uniform(-0.08, 0.08),
        )
        saplings += 1

    # Dense far tree wall: hides the terrain rim and closes the horizon.
    for i in range(26):
        angle = TAU * i / 26 + rng.uniform(-0.05, 0.05)
        radius = rng.uniform(28.0, 42.0)
        x = math.cos(angle) * radius
        z = math.sin(angle) * radius - 4.0
        if abs(x - path_center(z)) < 4.5 and z > -26.0:
            x += 5.0
        tree(
            veg,
            f"wall_tree_{i:02d}",
            x,
            z,
            rng.uniform(10.0, 19.0),
            rng.uniform(0.30, 0.65),
            rng.uniform(5.0, 9.0),
            rng,
            hero=True,
            lean=rng.uniform(-0.04, 0.04),
        )

    # Mid-distance trees close the gaps below the far canopy line, leaving
    # the trail's vanishing point open.
    placed = 0
    attempts = 0
    while placed < 20 and attempts < 600:
        attempts += 1
        angle = TAU * rng.random()
        radius = rng.uniform(17.0, 27.0)
        x = math.cos(angle) * radius
        z = math.sin(angle) * radius - 4.0
        if abs(x - path_center(z)) < 7.0 and -24.0 < z < 8.0:
            continue
        if any(math.hypot(x - ox, z - oz) < 3.0 for ox, oz in occupied):
            continue
        occupied.append((x, z))
        tree(
            veg,
            f"midwall_tree_{placed:02d}",
            x,
            z,
            rng.uniform(5.5, 10.0),
            rng.uniform(0.18, 0.36),
            rng.uniform(3.5, 6.0),
            rng,
            hero=False,
            lean=rng.uniform(-0.06, 0.06),
        )
        placed += 1

    # Palms, bananas, tree ferns, strangler figs and bamboo.
    palms = [
        (5.2, 9.2), (-5.4, 10.4), (7.2, -1.0), (-8.0, -16.0),
        (10.0, -20.0), (-11.0, -9.0), (13.0, 3.0), (3.6, 14.0),
        (-7.5, 4.5), (9.5, 8.0), (-12.0, -2.0), (0.5, -24.0),
        (14.0, -10.0),
    ]
    for i, (x, z) in enumerate(palms):
        palm(veg, f"palm_{i:02d}", x, z, rng.uniform(7.5, 12.5), rng)
    bananas = [
        (6.2, 3.2), (-6.0, 4.0), (6.0, -12.5), (-6.8, -13.0),
        (8.0, -7.0), (-4.5, -18.0), (11.0, 1.0), (-10.0, 8.0),
        (5.5, 12.0), (-7.5, -24.0), (12.0, -15.0), (-2.5, -22.0),
        (7.8, 13.5), (-13.0, -7.0), (3.0, -27.0),
    ]
    for i, (x, z) in enumerate(bananas):
        banana(veg, f"banana_{i:02d}", x, z, rng.uniform(2.2, 3.8), rng)
    tree_ferns = [
        (-4.0, 2.0), (4.8, -1.0), (-5.5, -7.0), (6.2, -10.0),
        (-2.8, -4.0), (8.5, -18.0), (-10.0, -12.0), (2.0, -18.0),
        (10.0, 5.0), (-12.0, 14.0), (4.5, 9.0), (-7.0, -1.0),
    ]
    for i, (x, z) in enumerate(tree_ferns):
        tree_fern(veg, f"tree_fern_{i:02d}", x, z, rng.uniform(2.4, 5.0), rng)
    figs = [
        (-9.0, -14.0, 17.0, 0.72, 8.5),
        (11.0, -8.0, 15.5, 0.66, 8.0),
        (-13.0, 6.0, 18.0, 0.78, 9.0),
        (15.0, -2.0, 16.5, 0.70, 8.4),
    ]
    for i, (x, z, h, r, w) in enumerate(figs):
        strangler_fig(veg, f"fig_{i:02d}", x, z, h, r, w, rng)
    # A handful of emergents break through the main canopy.
    emergents = [
        (-16.0, -24.0, 25.0, 1.0, 12.0),
        (18.0, -28.0, 27.0, 1.1, 13.0),
        (-22.0, 4.0, 23.0, 0.95, 11.0),
        (24.0, 10.0, 26.0, 1.05, 12.5),
        (6.0, -32.0, 24.0, 0.9, 11.0),
    ]
    for i, (x, z, h, r, w) in enumerate(emergents):
        tree(
            veg, f"emergent_{i:02d}", x, z, h, r, w, rng,
            hero=True, emergent=True, canopy_mesh=CANOPY_BROAD_MESH,
            lean=rng.uniform(-0.04, 0.04),
        )
    for i, (x, z) in enumerate([(-8.2, -5.0), (5.2, -18.5), (-9.0, 2.0)]):
        bamboo(veg, f"bamboo_{i}", x, z, rng)

    # Understory: ferns, shrubs, broadleaf plants.
    for i in range(205):
        for attempt in range(40):
            x = rng.uniform(-32.0, 32.0)
            z = rng.uniform(-32.0, 24.0)
            if plant_ok(x, z, path_clear=1.30, stream_clear=2.2, player_clear=2.8):
                break
        else:
            continue
        fern(veg, f"fern_{i:02d}", x, z, rng.uniform(0.90, 1.65), rng)
    for i in range(115):
        for attempt in range(40):
            x = rng.uniform(-34.0, 34.0)
            z = rng.uniform(-34.0, 24.0)
            if plant_ok(x, z, path_clear=1.70, stream_clear=2.8, player_clear=3.2):
                break
        else:
            continue
        shrub(veg, f"shrub_{i:02d}", x, z, rng.uniform(1.0, 2.2), rng, light=(i % 4 == 0))
    for i in range(72):
        for attempt in range(40):
            x = rng.uniform(-26.0, 26.0)
            z = rng.uniform(-28.0, 22.0)
            if plant_ok(x, z, path_clear=1.55, stream_clear=2.6, player_clear=3.0):
                break
        else:
            continue
        broadleaf(veg, f"broadleaf_{i:02d}", x, z, rng.uniform(0.75, 1.25), rng)

    # Grass on the banks and path edges.
    for i in range(195):
        for attempt in range(50):
            x = rng.uniform(-30.0, 30.0)
            z = rng.uniform(-30.0, 24.0)
            near_stream = abs(z - stream_center(x)) < 5.0
            near_path = abs(x - path_center(z)) < 3.0
            if not (near_stream or near_path):
                continue
            if abs(z - stream_center(x)) < 1.55 or abs(x - path_center(z)) < 1.05:
                continue
            if math.hypot(x - SPAWN[0], z - SPAWN[2]) < 2.2:
                continue
            break
        else:
            continue
        grass_tuft(veg, f"grass_{i:02d}", x, z, rng.uniform(0.45, 0.95), rng)

    # Rocks along the stream and clearing.
    for i in range(42):
        for attempt in range(50):
            x = rng.uniform(-28.0, 28.0)
            z = rng.uniform(-28.0, 22.0)
            d_stream = abs(z - stream_center(x))
            if d_stream < 1.8 or d_stream > 5.5:
                continue
            if abs(x - path_center(z)) < 1.8:
                continue
            break
        else:
            continue
        rock(veg, f"rock_stream_{i:02d}", x, z, rng.uniform(0.35, 1.25), rng, collider=(i < 6))
    for i in range(12):
        x = rng.uniform(-20.0, 20.0)
        z = rng.uniform(-20.0, 20.0)
        if abs(x - path_center(z)) < 2.4 or abs(z - stream_center(x)) < 3.0:
            continue
        rock(veg, f"rock_floor_{i:02d}", x, z, rng.uniform(0.4, 1.0), rng, collider=(i < 3))

    fallen_log(veg, "fallen_log_west", -5.2, -3.0, 5.5, 0.34, 1.15, rng)
    fallen_log(veg, "fallen_log_east", 7.5, -7.5, 4.8, 0.28, 2.1, rng)
    fallen_log(veg, "fallen_log_bank", -7.5, -14.0, 6.5, 0.38, 0.1, rng)
    for i, (x, z, r, h) in enumerate([(-1.8, 12.0, 0.35, 0.55), (6.0, 8.0, 0.45, 0.75), (-8.0, 8.5, 0.5, 0.9), (4.0, -22.0, 0.4, 0.6)]):
        stump(veg, f"stump_{i}", x, z, r, h, rng)

    for i, (x, z) in enumerate([(-2.2, 4.2), (2.4, 1.0), (-4.6, -3.5), (3.4, -8.5),
                                 (-1.6, -16.0), (5.8, 2.4), (-6.0, -18.0), (2.0, 15.0),
                                 (8.5, -3.0), (-9.5, -3.0), (1.2, -24.0), (-3.0, -25.0)]):
        flower_cluster(veg, f"flowers_{i:02d}", x, z, rng)

    for i in range(125):
        for attempt in range(40):
            x = rng.uniform(-34.0, 34.0)
            z = rng.uniform(-36.0, 24.0)
            if plant_ok(x, z, path_clear=1.15, stream_clear=1.9, player_clear=2.2):
                break
        else:
            continue
        ground_cover(veg, f"ground_cover_{i:02d}", x, z, rng.uniform(0.7, 1.6), rng)

    for i in range(130):
        for attempt in range(40):
            x = rng.uniform(-34.0, 34.0)
            z = rng.uniform(-36.0, 24.0)
            if plant_ok(x, z, path_clear=1.0, stream_clear=1.8, player_clear=1.8):
                break
        else:
            continue
        moss_patch(veg, f"moss_{i:02d}", x, z, rng.uniform(0.8, 2.4), rng)

    # Foreground framing plants (small, just inside the camera's view edges).
    fern(veg, "fern_hero_base_west", -4.8, 4.6, 1.45, rng)
    fern(veg, "fern_hero_base_east", 4.9, 2.4, 1.30, rng)
    broadleaf(veg, "broadleaf_hero_base_west", -2.8, 6.9, 1.05, rng)
    broadleaf(veg, "broadleaf_hero_base_east", 2.6, 4.4, 0.95, rng)
    fern(veg, "fern_frame_left", -1.55, 9.3, 1.25, rng)
    fern(veg, "fern_frame_right", 1.75, 9.6, 1.15, rng)
    shrub(veg, "shrub_frame_left", -2.9, 8.8, 1.1, rng, light=True)
    shrub(veg, "shrub_frame_right", 2.9, 7.7, 0.95, rng)

    player = build_character(None)

    spawn_ground = terrain_height(SPAWN[0], SPAWN[2])
    camera_node = attach(
        None,
        make_node("camera", (SPAWN[0], spawn_ground + 2.2, SPAWN[2] + 5.0)),
    )
    camera_node["components"]["Camera3D"] = {
        "position": [round(SPAWN[0], 3), round(spawn_ground + 2.2, 3), round(SPAWN[2] + 5.0, 3)],
        "target": [round(SPAWN[0], 3), round(spawn_ground + 1.4, 3), round(SPAWN[2], 3)],
        "fov_degrees": 52.0,
        "near_plane": 0.05,
        "far_plane": 180.0,
        "follow": True,
    }

    campfire(veg, "campfire", -3.4, 2.2, rng)
    lantern(veg, "trail_lantern", 2.7, -3.2, 1.8, rng)
    firefly(veg, "firefly_00", -1.8, 0.9, -7.0, rng)
    firefly(veg, "firefly_01", 3.2, 1.1, -5.5, rng)
    firefly(veg, "firefly_02", -5.0, 0.8, 5.2, rng)

    insects = attach(world, make_node("insects"))
    build_insects(insects, rng)

    # Ambient animation holders.
    ambience = attach(None, make_node("canopy_ambience", (0.0, 0.0, 0.0)))
    ambience["components"]["Animator"] = {
        "clip_id": "jungle_sway",
        "speed": 0.8,
        "loop": True,
        "playing": True,
    }
    player["components"]["Animator"] = {
        "clip_id": "jungle_idle",
        "speed": 1.0,
        "loop": True,
        "playing": True,
    }

    scene = {
        "version": 3,
        "name": "jungle",
        "settings": {
            "light_direction": [-0.45, 0.58, 0.45],
            "light_color": [1.0, 0.84, 0.62],
            "light_intensity": 2.20,
            "ambient": [0.10, 0.13, 0.10],
            "exposure": 1.42,
            "sky_color": [0.30, 0.46, 0.58],
            "ground_color": [0.055, 0.10, 0.035],
            "shadows_enabled": True,
            "fxaa_enabled": True,
            "sky_enabled": True,
            "horizon_warmth": 0.90,
            "fog_enabled": True,
            "fog_density": 0.0055,
            "bounce_enabled": True,
            "bounce_color": [0.35, 0.28, 0.18],
            "bounce_intensity": 0.32,
            "ssao_enabled": True,
            "environment_intensity": 0.70,
        },
        "entities": serialize_nodes(ROOTS),
    }
    return scene, player


def serialize_nodes(nodes):
    out = []
    for node in nodes:
        entity = {"name": node["name"], "components": node["components"]}
        if node["children"]:
            entity["children"] = serialize_nodes(node["children"])
        out.append(entity)
    return out


# --------------------------------------------------------------------------
# animations
# --------------------------------------------------------------------------


def build_animations(player):
    avatar_guid = player["_avatar_guid"]
    idle = {
        "name": "jungle_idle",
        "tracks": [
            {
                "guid": avatar_guid,
                "property": "scale",
                "keyframes": [
                    {"t": 0.0, "v": [1.0, 1.0, 1.0]},
                    {"t": 2.1, "v": [1.004, 1.008, 1.004]},
                    {"t": 4.4, "v": [1.0, 1.0, 1.0]},
                ],
            },
            {
                "guid": avatar_guid,
                "property": "position",
                "keyframes": [
                    {"t": 0.0, "v": [0.0, 0.0, 0.0]},
                    {"t": 2.1, "v": [0.0, 0.004, 0.0]},
                    {"t": 4.4, "v": [0.0, 0.0, 0.0]},
                ],
            },
        ],
    }
    with open(os.path.join(ANIMATION_DIR, "jungle_idle.json"), "w") as handle:
        json.dump(idle, handle, indent=2)
        handle.write("\n")
    print("  animation jungle_idle.json")

    # Canopy sway: rotate a handful of hero canopy blobs with offset phases.
    sway_targets = []
    for name in GUID_INDEX:
        if "_canopy" in name and any(
            name.startswith(prefix)
            for prefix in ("tree_west_hero", "tree_east_hero", "tree_west_mid")
        ):
            sway_targets.append(name)
    sway_targets = sway_targets[:8]
    tracks = []
    for index, name in enumerate(sway_targets):
        phase = index * 0.65
        amp = 0.010 + 0.003 * (index % 3)
        keys = []
        for step in range(5):
            t = step * 1.5
            z_rot = amp * math.sin(phase + step * 1.1)
            keys.append({"t": t, "v": [0.0, 0.0, round(z_rot, 5)]})
        tracks.append(
            {
                "guid": GUID_INDEX[name],
                "property": "rotation",
                "keyframes": keys,
            }
        )
    sway = {"name": "jungle_sway", "tracks": tracks}
    with open(os.path.join(ANIMATION_DIR, "jungle_sway.json"), "w") as handle:
        json.dump(sway, handle, indent=2)
        handle.write("\n")
    print("  animation jungle_sway.json")


# --------------------------------------------------------------------------
# assets registry / validation / main
# --------------------------------------------------------------------------


def update_assets_registry():
    registry = {"version": 1, "assets": {}}
    if os.path.exists(ASSETS_PATH):
        with open(ASSETS_PATH) as handle:
            try:
                existing = json.load(handle)
                if isinstance(existing, dict) and isinstance(existing.get("assets"), dict):
                    registry["assets"] = existing["assets"]
            except json.JSONDecodeError:
                pass
    assets = {
        key: value
        for key, value in registry["assets"].items()
        if not key.startswith("jungle_")
    }
    assets["sample_bob"] = "animations/sample_bob.json"
    registry["assets"] = assets
    assets["jungle_terrain"] = "assets/jungle_terrain.glb"
    assets["jungle_path"] = "assets/jungle_path.glb"
    assets["jungle_stream"] = "assets/jungle_stream.glb"
    assets["jungle_ground"] = "assets/textures/jungle_ground.png"
    assets["jungle_path_tex"] = "assets/textures/jungle_path.png"
    assets["jungle_avatar"] = "assets/jungle_avatar.glb"
    assets["jungle_avatar_atlas"] = "assets/textures/jungle_avatar_atlas.png"
    assets["jungle_insect_wings"] = "assets/textures/jungle_insect_wings.png"
    assets["jungle_palm_bark"] = "assets/textures/jungle_palm_bark.png"
    assets["jungle_bark_smooth"] = "assets/textures/jungle_bark_smooth.png"
    assets["jungle_leaf_grain"] = "assets/textures/jungle_leaf_grain.png"
    assets["jungle_leaf_grain_light"] = "assets/textures/jungle_leaf_grain_light.png"
    assets["jungle_bark"] = "assets/textures/jungle_bark.png"
    assets["jungle_bark_pale"] = "assets/textures/jungle_bark_pale.png"
    assets["jungle_rock"] = "assets/textures/jungle_rock.png"
    assets["jungle_water"] = "assets/textures/jungle_water.png"
    assets["jungle_canvas"] = "assets/textures/jungle_canvas.png"
    for key, _file, _strength, _blur in NORMAL_DEFS:
        assets[key + "_normal"] = "assets/textures/" + key + "_normal.png"
    assets["jungle_idle"] = "assets/animations/jungle_idle.json"
    assets["jungle_sway"] = "assets/animations/jungle_sway.json"
    with open(ASSETS_PATH, "w") as handle:
        json.dump(registry, handle, indent=2)
        handle.write("\n")
    for stale in (
        "jungle_leaves.png",
        "jungle_leaves_light.png",
        "jungle_fern.png",
        "jungle_leaf_atlas.png",
        "jungle_leaf_atlas_light.png",
        "jungle_khaki.png",
        "jungle_olive.png",
        "jungle_leather.png",
        "jungle_skin.png",
        "jungle_metal.png",
    ):
        path = os.path.join(TEXTURE_DIR, stale)
        if os.path.exists(path):
            os.remove(path)
    print(f"  registry {os.path.relpath(ASSETS_PATH, ROOT)}")


def validate(scene):
    counts = {}
    total_entities = 0

    def walk(entities):
        nonlocal total_entities
        for entity in entities:
            total_entities += 1
            for key in entity["components"]:
                counts[key] = counts.get(key, 0) + 1
            walk(entity.get("children", []))

    walk(scene["entities"])
    return total_entities, counts


def main():
    print(f"jungle generator: seed {SEED}")
    build_textures()
    os.makedirs(ANIMATION_DIR, exist_ok=True)
    print("meshes:")
    build_terrain_mesh()
    build_path_mesh()
    build_stream_mesh()
    build_leaf_meshes()
    build_leaf_tuft_mesh()
    build_insect_wings()
    avatar_verts, avatar_tris = build_avatar_assets(
        os.path.join(TEXTURE_DIR, "jungle_avatar_atlas.png"),
        os.path.join(ROOT, "assets", "jungle_avatar.glb"),
        os.path.join(TEXTURE_DIR, "jungle_avatar_face_source.webp"),
    )
    print(f"  avatar {avatar_verts} verts {avatar_tris} tris")

    scene, player = build_scene()
    build_animations(player)
    update_assets_registry()
    with open(SCENE_PATH, "w") as handle:
        json.dump(scene, handle, indent=2)
        handle.write("\n")
    total, counts, = validate(scene)
    print(
        f"scene: {total} entities, "
        f"{counts.get('MeshSource', 0)} meshes, "
        f"{counts.get('RigidBody', 0)} physics bodies"
    )
    print(f"wrote {os.path.relpath(SCENE_PATH, ROOT)}")


if __name__ == "__main__":
    main()

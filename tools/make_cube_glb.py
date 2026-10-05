#!/usr/bin/env python3
"""Generates assets/cube.glb - a small valid binary glTF 2.0 cube.

Run from the repo root:  python3 tools/make_cube_glb.py

Kept in the repo so the demo asset is reproducible without checking in a
binary blob by hand.
"""

import json
import struct
import os

H = 0.5  # half extent

# Six faces, four vertices each, so every face gets a flat normal.
faces = [
    ((0, 0, 1), [(-H, -H, H), (H, -H, H), (H, H, H), (-H, H, H)]),
    ((0, 0, -1), [(H, -H, -H), (-H, -H, -H), (-H, H, -H), (H, H, -H)]),
    ((1, 0, 0), [(H, -H, H), (H, -H, -H), (H, H, -H), (H, H, H)]),
    ((-1, 0, 0), [(-H, -H, -H), (-H, -H, H), (-H, H, H), (-H, H, -H)]),
    ((0, 1, 0), [(-H, H, H), (H, H, H), (H, H, -H), (-H, H, -H)]),
    ((0, -1, 0), [(-H, -H, -H), (H, -H, -H), (H, -H, H), (-H, -H, H)]),
]

positions, normals, indices = [], [], []
for normal, corners in faces:
    base = len(positions)
    positions.extend(corners)
    normals.extend([normal] * 4)
    indices.extend([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3])


def pad4(data, fill=b"\x00"):
    while len(data) % 4:
        data += fill
    return data


pos_bytes = struct.pack("<%df" % (len(positions) * 3), *[c for p in positions for c in p])
nrm_bytes = struct.pack("<%df" % (len(normals) * 3), *[c for n in normals for c in n])
idx_bytes = struct.pack("<%dH" % len(indices), *indices)

buffer = pos_bytes + nrm_bytes + idx_bytes
while len(buffer) % 4:
    buffer += b"\x00"

gltf = {
    "asset": {"version": "2.0", "generator": "lotus make_cube_glb.py"},
    "scene": 0,
    "scenes": [{"nodes": [0]}],
    "nodes": [{"mesh": 0, "name": "Cube"}],
    "meshes": [
        {
            "name": "Cube",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2}
            ],
        }
    ],
    "buffers": [{"byteLength": len(buffer)}],
    "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes), "target": 34962},
        {
            "buffer": 0,
            "byteOffset": len(pos_bytes),
            "byteLength": len(nrm_bytes),
            "target": 34962,
        },
        {
            "buffer": 0,
            "byteOffset": len(pos_bytes) + len(nrm_bytes),
            "byteLength": len(idx_bytes),
            "target": 34963,
        },
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": len(positions),
            "type": "VEC3",
            "min": [-H, -H, -H],
            "max": [H, H, H],
        },
        {
            "bufferView": 1,
            "componentType": 5126,
            "count": len(normals),
            "type": "VEC3",
        },
        {
            "bufferView": 2,
            "componentType": 5123,
            "count": len(indices),
            "type": "SCALAR",
        },
    ],
}

json_bytes = pad4(json.dumps(gltf, separators=(",", ":")).encode(), b" ")
bin_bytes = buffer

total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
glb = b"glTF" + struct.pack("<II", 2, total)
glb += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
glb += struct.pack("<II", len(bin_bytes), 0x004E4942) + bin_bytes

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out_path = os.path.join(root, "assets", "cube.glb")
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "wb") as handle:
    handle.write(glb)

print(f"wrote {out_path} ({len(glb)} bytes, {len(positions)} vertices, {len(indices) // 3} triangles)")

# LotusEngine

A small C++ game engine: ECS scenes, SDL GPU renderer with Hi-Z occlusion, Jolt
physics, transform animation, region streaming, Lua scripts, and an in-engine
editor.

## Quick start

```bash
git submodule update --init --recursive
cmake -B build -G Ninja
cmake --build build -j"$(nproc)"
cd build && ./bin/lotus
```

Working directory should be `build/` so `scenes/`, `assets/`, and `prefabs/`
resolve correctly.

### Sample hub (`scenes/main.json`)

- **WASD** — move the `player` character (Shift to sprint).
- **F1** — editor (viewport pick uses physics when possible).
- **F2** — console (`help`, `scene.switch`, `scene.load_async`, …).
- **Ctrl+Q** — quit.
- **F2** `game.save` / `game.load` — player pose + active scene name (`saves/game.json`).

Movement is **camera-relative** on the XZ plane (WASD strafe relative to the view).

The gold **bob_prop** sphere plays the `sample_bob` animation clip from
`assets/`. Walk east (~40 m) toward the sign; when the camera enters another
region from `scenes/regions.json`, additive scenes load (one region per frame by
default).

### Tests

```bash
cd build && ./bin/lotus_tests
```

GPU occlusion tests: `./bin/lotus_tests --gtest_filter=OcclusionGpuTest.*`

Regenerate shaders (requires `glslangValidator`):

```bash
./shaders/build-shaders.sh
```

### Environment

| Variable | Effect |
|----------|--------|
| `LOTUS_NO_UI=1` | Hide editor UI |
| `LOTUS_MAX_FRAMES=N` | Exit after N frames (automation) |
| `LOTUS_CONSOLE="cmd; …"` | Run console commands at startup |

## Docs

Design specs live under `docs/superpowers/specs/`.

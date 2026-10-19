# AGENTS.md — LotusEngine vibe-coding loop

This repo is an engine you drive headless. The loop is: **read → modify → verify**.

## 1. Read the world

Scenes live in `scenes/*.json`, format version 3 (`"version": 3` at the top).
An entity is `{"name": ..., "components": {...}}`, optionally with `children`.

Component names (these exact strings; `Name` is the `"name"` field,
`Hierarchy` is nesting — neither is a component key):

Animator, BoxCollider, Camera2D, Camera3D, CharacterController, EntityRef,
Lod, Material, MeshCollider, MeshSource, RigidBody, SceneGuid, SceneOrigin,
Script, SphereCollider, Sprite, Transform

Peek at one entity first, e.g. `python3 -c "import json;
d=json.load(open('scenes/main.json')); print(d['entities'][0])"`.

The top-level `"settings"` block (light direction/color/intensity, ambient,
exposure, sky/ground colors, `shadows_enabled`, `fxaa_enabled`,
`sky_enabled`, `horizon_warmth`) is optional — absent keys keep defaults.

## 2. Modify

Prefer the console verbs over hand-editing JSON (they validate as they go).
All entity verbs act on the **active scene** and answer `error: ...` when
there is no active scene, the entity is unknown, the component is unknown,
or the JSON is bad.

```
entity.list                                  # names, one per line
entity.get <name>                            # {"name", "components"} as JSON
entity.spawn <name>                          # create; confirms "spawned <name>"
entity.destroy <name>                        # missing -> error: no entity named <name>
entity.set <name> <Component> <json>         # e.g. entity.set box2 Transform {"position":[1,2,3]}
entity.unset <name> <Component>              # Transform/Hierarchy/Name/SceneGuid refuse with error:
                                             # they are structural and every entity carries them
```

Example session:

```
entity.spawn box2
entity.set box2 Transform {"position":[1,2,3]}
entity.set box2 Material {"base_color":[0.8,0.1,0.1],"roughness":0.5}
entity.get box2
entity.unset box2 Material
entity.destroy box2
```

Useful neighbors: `scene.list`, `scene.switch <name>`, `scene.save`
(write the active scene back to disk), `scene.reload`, `scene.merge <path>`,
`prefab.list`, `prefab.instantiate <name> [parent]`, `script.reload`,
`asset.list`, `keybinds.list`, `help`.

Two ways to run commands without a window:

* Env (interactive run): `LOTUS_CONSOLE="entity.list; entity.get box2"`.
* Headless (preferred): `--exec "entity.spawn box2; entity.set box2
  Transform {\"position\":[1,2,3]}"` — runs after scene load + script sync,
  before ticks. Splitting on `;` is identical in both.

## 3. Verify

```
lotus --headless --ticks N --screenshot out.ppm --dump-state state.json
```

* `--ticks N` steps the simulation at 60 Hz fixed dt, then exits.
* `--screenshot out.ppm` renders one binary-PPM frame (software Vulkan /
  lavapipe is enough — no GPU needed; empty skips all GPU work).
* `--dump-state state.json` writes the scene back as v3 JSON; diff it
  against `scenes/<name>.json` to confirm your edits landed.
* `--scene NAME` (default `main`), `--scene-dir DIR` (default `scenes`),
  `--assets-dir DIR` (default `assets`), `--width W --height H`
  (default 1280x720), `--seed S` (default 42).

The seed feeds `std::rand`, which is also what Lua's `math.random` draws
from — same seed, same run. Settings toggles such as `shadows_enabled`,
`fxaa_enabled`, and `sky_enabled` can be flipped in the JSON to isolate
rendering vs. logic issues.

## Exit-code contract

* `0` — run completed (`... result=ok` on stdout).
* `1` — runtime failure: scene would not load, script sync failed,
  screenshot/dump failed, or `--exec` stopped at the first command whose
  result starts with `error: ` (the failing command is named in the message).
* `2` — CLI misuse: unknown flag, missing value, or bad number
  (usage is printed to stderr).

So: `--exec` aborts on the first `error: ` result — check exit codes before
trusting `state.json` or `out.ppm`.

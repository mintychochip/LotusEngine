# Scene subsystem: prefabs and stable identity

## Goal

Close the largest gap in the scene subsystem — reusable objects — by adding **prefabs and instantiation**, on top of the **stable entity identity** they require, plus two foundation defects that would otherwise block both.

This milestone is Workstreams 0–2. Everything else the subsystem lacks is listed under "Deferred" with a reason, so the scope is a decision rather than a wish list.

A prefab is an existing serialized subtree, so the codec that already round-trips a scene round-trips a prefab unchanged. That is the concrete win and the reason this is the right first milestone.

## Repo constraints

Facts about the existing code, each verified in source. They shape the design; none is speculative.

- **ECS**: EnTT (submodule, `skypjack/entt`) behind a 26-line facade, `include/lotus/ecs.hpp`. `Entity` is `entt::entity`, a versioned handle: a stale handle from a destroyed entity never aliases a recycled one. `Registry` is `entt::registry`.
- **Atomicity is free and prefabs inherit it**: `scene_from_json` (`src/scene/serialization.cpp`) parses into a temporary `Scene` and commits with `scene = std::move(temp)`. Three tests already assert a malformed document cannot damage the live scene (`MalformedJsonLeavesSceneUntouched`, `BadShapeLeavesSceneUntouched`, `UnsupportedVersionIsRejected`).
- **Codec registry**: `ComponentCodecs::add` is already public, and `to_json_string` / `scene_from_json` drive it. Eight components are registered: `Transform`, `Sprite`, `Camera2D`, `Script`, `MeshSource`, `Material`, `Camera3D`, `Lod`. Keys live in a `std::map`, so writer order is deterministic.
- **Codecs are not extensible from outside the engine**: `default_component_codecs()` returns a function-local `static const ComponentCodecs` built inside a lambda (`serialization.cpp:239-242`). There is no registration API. This does not block prefabs (they use the built-in eight) and is deferred.
- **Tolerant reader**: unknown components are skipped, never fatal; omitted fields keep defaults; derived fields (`Transform::world`, `Hierarchy`, `Name`) are never written.
- **Hierarchy is nesting-only**: parent/child with world-transform propagation, cycle rejection, and recursive destroy (`scene.cpp`). There is no persistent identity, so nothing can reference an entity that is not its parent or child.
- **Assets are bare paths**: `MeshSource { std::string path; }` and `Script { std::string path; }`.
- **Versioning rejects the future**: `kSceneFormatVersion = 1`, and `scene_from_json` rejects any version `> kSceneFormatVersion` (`serialization.cpp:539-540`) with no migration path.
- **Build**: C++23 (`CMakeLists.txt:7`). `CMakeLists.txt` uses `GLOB_RECURSE ... CONFIGURE_DEPENDS` for `src/*.cpp` (line 49) and `test/*.cpp` (line 97), so a new `.cpp` **anywhere under `src/` or `test/`** — including subdirectories — is picked up automatically. No CMake edits.

## Rules

Invariants that must survive this milestone. They are constraints, not preferences.

- **No breaking the baseline.** `build/bin/lotus_tests` is at **148 passing tests**. Never leave it lower. Existing public signatures are preserved, or migrated with every caller updated in the same change. No dead code, no shims, no aliases.
- **Loads stay atomic.** Every new load or instantiate path builds into a temporary and swaps. A malformed prefab, a missing asset, or an unresolved reference must leave the live scene untouched.
- **Unknown components stay non-fatal.** Skipping an unrecognized component remains the behaviour; a prefab containing one still instantiates.
- **Derived fields are never written.** `Transform::world`, `Hierarchy`, `Name`, and any new derived state stay out of the document.
- **Identity stays versioned.** `Entity` remains `entt::entity`; nothing replaces the ECS or introduces raw indices.
- **The format evolves forward only.** A file written by version N loads under N+1 through an explicit migration. Reject only versions newer than the current.
- **Determinism.** Serialized key order, `SceneManager::names()`, and any enumerated output are deterministic; no dependence on `unordered_map` iteration.
- **One-directional failure.** A missing asset or unresolved reference is reported (`last_mesh_error()`-style) and drawn as nothing. It never corrupts the scene and never crashes the editor.
- **Occlusion, LOD, and frustum culling are untouched.** No per-frame cost is added to `Scene::build_vertices` beyond load-time reference resolution.

## Milestone design

### 0. Foundation defects (do first)

Both block the milestone: identity needs a serialized key that survives a reload, and a format change needs a migration path.

- **Sort `names()`.** The header promises "Names of all scenes, sorted" (`scene_manager.hpp:33`) but the implementation iterates an `unordered_map` with no sort (`scene_manager.cpp`), so order is arbitrary per run and `scene.list` output is non-deterministic. Add `std::sort`; no caller changes.
- **Add a migration hook.** Bump `kSceneFormatVersion` to 2 and add `bool migrate_scene_json(nlohmann::json&, int from, int to)`. Version 1 → 2 is a no-op that proves the path exists. Change the version guard to reject only `version > kSceneFormatVersion`.

**Files**: `src/scene/scene_manager.cpp`, `src/scene/serialization.cpp`, `include/lotus/scene/serialization.hpp`.
**Tests**: `names()` is sorted and stable across runs; a version-1 document migrates to 2 and loads.

### 1. Stable identity and references

The enabling change. Without a persistent identity, a prefab instance has nothing to point at, and a reference cannot survive a save/load.

- Add `SceneGuid { uint64_t value{0}; }`, assigned in `Scene::create`, serialized as a hex string. `0` means "unassigned" and is regenerated on load.
- Add `EntityRef { SceneGuid target; }`, naming another entity by GUID rather than by handle.
- Add `Scene::resolve_refs()`, run after load and after any structural edit. It maps GUIDs to handles and reports unresolved references through the existing `last_mesh_error()` channel rather than throwing.
- Register both through the codec registry.

**Files**: `include/lotus/scene/components.hpp`, `src/scene/scene.cpp`, `src/scene/serialization.cpp`.
**Tests**: a GUID round-trips through save/load; a reference to a missing GUID is reported and the scene stays valid; a reference survives reordering entities in the file.

### 2. Prefabs and instancing

- A prefab is a **scene fragment** stored under `prefabs/<name>.json`, in the same versioned format as a scene. Reusing the codec path is deliberate: one parser, one migration path, no second format to maintain.
- `Entity instantiate(const Prefab&, Entity parent = NullEntity)` clones the fragment's entities and components into the target scene, assigns fresh GUIDs, and rewrites internal `EntityRef`s to the new GUIDs.
- Overrides: instantiation takes an optional transform. Keep the first version to transform-only; a general override list is deferred.
- Editor: "Create prefab from selection" and an instantiate entry in the Hierarchy panel.

**Files**: new `include/lotus/scene/prefab.hpp`, `src/scene/prefab.cpp`; `src/ui/editor.cpp`.
**Tests**: instantiating twice yields independent entities (mutating one does not touch the other); internal references inside a prefab are rewritten per instance; a prefab referencing a missing asset instantiates with a reported error and no corruption.

## Deferred

Deferred when this milestone landed, and since delivered in their own commits
under `2026-10-05-scene-composition-design.md`. Kept here as the record of what
was knowingly left out at the time:

| Was deferred | Status now |
|---|---|
| Additive / multi-scene composition, `scene.switch` | Delivered: `load_scene_additive`, `scene.switch`, `scene.load`, `scene.merge`. |
| Async / non-blocking load | Delivered: `AsyncSceneLoad`, start-then-poll, never wait. |
| Asset reference indirection | Delivered: optional `id` on `MeshSource` and `Script`, resolved through the registry; absent id means "use the path". |
| Undo scalability | Delivered: `SceneHistory` byte capacity alongside the step count. |
| General prefab overrides | Delivered: `PrefabOverrides` (root transform + component documents by codec name). |
| Prefab nesting semantics | Pinned by tests; a nested capture flattens into one document, and a reference outside the fragment is reported, not silently dropped. |
| Per-scene environment | Delivered: `SceneSettings`, applied every frame when a scene carries one. |
| Scene streaming / tiles | Partly delivered: `destroy_by_origin` plus `ResidencyPolicy`. A frame-budgeted region scheduler is still open. |
| Networking or scene authority | Still out of scope. |


Delivered in this milestone, beyond the three workstreams: `prefab.list`,
`prefab.save <name> <entity>` and `prefab.instantiate <name> [parent]` console
commands, and a Prefabs editor panel (create from selection, instantiate) wired
through `Editor::PrefabControls`. Also `register_component_codec(name, write,
read)`, so game code can serialize component types the engine does not know.

## Tests

Add cases to the existing suites and keep the 148 baseline green. The properties that matter, in priority order:

1. **Atomicity**: no new load or instantiate path can damage the live scene. The subsystem already has this; it is the property most easily lost.
2. **Backward compatibility**: a version-1 scene file loads under version 2 through migration.
3. **Identity**: GUIDs round-trip and references survive reordering.
4. **Independence**: two instances of a prefab share nothing.
5. **Determinism**: `names()` and serialized key order are stable across runs.

## Build and verification

- Build: `cd build && cmake --build . -j"$(nproc)"`.
- Tests: `build/bin/lotus_tests`. Baseline **148 passing**; never lower.
- Run the app: `cd build && ./bin/lotus` (working directory is `build/`; `scenes/main.json` and `prefabs/` resolve there). `F1` editor, `F2` console, `Ctrl+Q` quit.
- No CMake edits. New `.cpp` files anywhere under `src/` or `test/` are globbed in automatically (`GLOB_RECURSE ... CONFIGURE_DEPENDS`).

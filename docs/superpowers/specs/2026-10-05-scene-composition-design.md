# Scene subsystem: composition

## Goal

Load a scene file **into** a live scene, additively, without replacing what is
already there — and switch between scenes from the console. This is the deferred
"additive / multi-scene composition, `scene.switch`" item from the prefabs
milestone, unblocked by the GUID work: without persistent identity, two merged
entity sets could not coexist or be told apart.

Scope is Workstreams 0–1 below. The rest of the deferred table is untouched.

## Repo constraints

Facts about the existing code, each verified in source.

- **Additive load is instantiation of a whole document.** `instantiate()` already
  parses a fragment atomically, assigns fresh GUIDs, remaps internal references,
  and rebuilds hierarchy. A scene document has the same shape, so merging is the
  same operation with a different source. Reusing it is deliberate; there is no
  second merge path to keep correct.
- **`scene_from_json` replaces.** It parses into a temporary and commits with
  `scene = std::move(temp)`, so it is atomic but destructive by definition. It
  cannot be the additive path.
- **Identity exists.** `SceneGuid` + `EntityRef` + `resolve_refs()` landed in the
  prefabs milestone. `Scene::resolve_refs()` advances the GUID counter past every
  loaded GUID, so two merged documents cannot collide.
- **One active scene.** `SceneManager` is a flat `unordered_map<string,
  unique_ptr<Scene>>`; `names()` is sorted. `activate()` switches; there is no
  additive path and no `scene.switch` command.
- **One file path.** `Application` holds a single `scene_path_`, and the hot
  reload watcher is keyed `"scene"`. Switching scenes must move both, or the
  watcher keeps reloading the file you left.
- **Build**: C++23, `GLOB_RECURSE ... CONFIGURE_DEPENDS` over `src/` and `test/`,
  so no CMake edits.

## Rules

- **Additive load is atomic.** A malformed document merges nothing. The target
  scene is untouched on any failure.
- **Fresh identity per merge.** Every merged entity gets a new GUID, and internal
  references are rewritten to the merged copies. Loading the same file twice
  yields two independent sets.
- **Merge never deletes.** Entities already in the target survive a merge, and
  their GUIDs are unchanged.
- **Switching is a pointer move, not a reload.** `activate()` must not re-read
  from disk; unsaved changes in the scene being left stay in memory.
- **The watcher follows the active scene.** After a switch, hot reload watches
  the newly active scene's file, or nothing.
- **Determinism.** Merged entities appear in document order, and `names()` stays
  sorted.
- **No occlusion, LOD, or frustum changes.** Merging is a load-time operation.

## Workstream 0: additive load

- `bool load_scene_additive(Scene& target, const std::string& text,
  std::string* error = nullptr)` in the scene layer. Parses into a temporary
  `Scene`, then merges every root through the same clone-and-remap path
  `instantiate()` uses, so identity and references behave identically.
- `bool load_scene_additive_from_file(Scene&, const std::string& path, ...)`.
- Refactor the shared clone-and-remap into one function so `instantiate` and the
  additive path cannot drift apart. That function is the seam; two copies of the
  remap logic would be the bug.

**Files**: `include/lotus/scene/prefab.hpp` (or a new `scene_merge.hpp`),
`src/scene/prefab.cpp`, `test/scene_merge_test.cpp`.

**Tests**: merging two documents yields the union of their entities with distinct
GUIDs; loading the same file twice yields two independent sets; internal
references in the merged document point within the merged copies; a malformed
document merges nothing and leaves the target's entities and GUIDs intact;
existing entities survive a merge with their GUIDs unchanged.

## Workstream 1: switching and command surface

- `scene.switch <name>` console command. Switches to a loaded scene if present,
  otherwise loads it from `<scene_dir>/<name>.json` and activates it. Reports
  rather than crashing when the file is missing.
- `scene.load <name> <path>` and `scene.merge <path>` for the additive path.
- On switch, `Application` moves `scene_path_` and re-keys the watcher to the new
  file, so hot reload follows the active scene instead of the abandoned one.
- Editor: the Scene File panel shows the active scene's name and file.

**Files**: `include/lotus/application/application.hpp`,
`src/core/application/application.cpp`, `src/ui/editor.cpp`.

**Tests**: `scene.switch` changes `active_name()`; switching to a missing scene
reports and leaves the current scene active; a switch does not reload from disk
(unsaved changes survive); after a switch the watcher watches the new path.

## Deferred

Unchanged from the prefabs milestone, and out of scope here:

| Deferred | Reason |
|---|---|
| Async / non-blocking load | Composition is correctness work; concurrency is a separate surface. |
| Asset reference indirection (`AssetRef`) | Needs its own format bump; unrelated to composition. |
| Undo scalability | Snapshot undo is fine for a level editor; profiling has not asked for more. |
| General prefab overrides | Needs a diff format and merge rule; its own design. |
| Prefab nesting semantics | Works incidentally today; pin the semantics before relying on it. |
| Per-scene environment | Renderer call sites and global state; unrelated. |
| Scene streaming / tiles | Open-world concern, not scene composition. |
| Entity scoping across merged scenes (a "scene tag" per entity) | Merging makes the union indistinguishable; tagging is only needed once something must query by origin. Not required for load/switch. |

## Build and verification

- Build: `cd build && cmake --build . -j"$(nproc)"`.
- Tests: `build/bin/lotus_tests`. Baseline is **168 passing**; never lower.
- Run: `cd build && ./bin/lotus`. Console via `F2` or `LOTUS_CONSOLE`.
- No CMake edits.

# Engine subsystems: registry, materials, physics, animation, wiring

## Goal

Five sub-projects, in dependency order:

1. **Asset registry** — a stable key to current path map, so moving a file on
   disk does not require editing every scene that referenced it.
2. **Textured materials** — UVs, textures, and per-material draw batching.
   This is a renderer restructure, not an addition.
3. **Physics** — Jolt, vendored as a submodule.
4. **Animation** — transform/keyframe clips. Skeletal is explicitly later.
5. **Wiring** — give the orphaned libraries a consumer or delete them.

## Repo constraints

These are facts about the existing code, not preferences. Each one shapes the
design.

- Geometry is uploaded **pre-transformed to world space** and drawn in **one
  draw call**. `GpuVertex` is position/normal/colour; `Scene::upload` fills one
  vertex buffer and `Scene::draw` issues one `SDL_DrawGPUPrimitives`. The Hi-Z
  spec depends on this shape.
- `Material` is a single `glm::vec3 base_color`. There are no UVs in
  `GpuVertex`, no samplers, and no texture binding anywhere in the renderer.
- `lit.frag` writes `out_color` with alpha hardcoded to `1.0` and
  `out_linear_depth` at location 1. The scene pass has **two** colour targets:
  the swapchain (or offscreen viewport) and an R32F pyramid stand-in.
- `MeshSource::id` and `Script::id` serialize but **nothing resolves them**.
  `AssetRegistry` is named in two doc comments and does not exist as a type.
- `resolve_mesh` handles `builtin:cube`, `builtin:sphere[:<segments>:<rings>]`,
  and `.glb` paths.
- `stb_image.h` is vendored and `src/stb_image.cpp` compiles it. Nothing calls
  it.
- The GLB loader parses POSITION, NORMAL (or computes them), and indices only.
  No TEXCOORD, no materials, no skins, no animations.
- `Application` owns the scene directory and the prefab directory. `Scene`
  does not know about files.
- New files under `src/` or `test/` are picked up automatically
  (`GLOB_RECURSE ... CONFIGURE_DEPENDS`). No CMake edit is needed for them.
- SDL's SPIR-V resource convention: vertex shaders use set 0 for sampled
  textures and set 1 for uniform buffers; fragment shaders use set 2 for
  sampled textures and set 3 for uniform buffers. `lit.vert` and `lit.frag`
  already follow this, so **set 2 is the correct home for an albedo texture**.

## Sub-project 1: Asset registry

### Design

`include/lotus/scene/assets.hpp`, `src/scene/assets.cpp`.

```cpp
class AssetRegistry {
public:
    bool load(const std::string& path, std::string* error = nullptr);
    bool save(const std::string& path, std::string* error = nullptr) const;

    const std::string* find(const std::string& id) const;
    void set(const std::string& id, std::string path);
    bool erase(const std::string& id);
    std::vector<std::string> names() const;   // sorted

    std::string resolve(const std::string& id,
                        const std::string& fallback) const;
    bool dangling(const std::string& id) const;
    std::size_t size() const;

private:
    std::unordered_map<std::string, std::string> paths_;
};
```

File format, written next to the assets it describes:

```json
{ "version": 1, "assets": { "cube": "assets/cube.glb" } }
```

Rules:

- `resolve(id, fallback)` returns the registry path when `id` is non-empty and
  known; otherwise `fallback` unchanged. An empty `id` always yields
  `fallback`, which is what every scene written before keys existed relies on.
- `dangling(id)` is true when `id` is non-empty and unknown. The caller reports
  it and still uses the fallback, so a scene never fails to load because of a
  missing key.
- `load` is atomic: a malformed document leaves the previous table intact.
- `names()` is sorted, so console output and the editor listing are
  deterministic.

### Wiring

- `Application` owns one `AssetRegistry`, loaded from
  `<assets dir>/assets.json`.
- `Scene` gains `void set_assets(const AssetRegistry*)` — non-owning,
  nullable. Null means "resolve by path", which is what every existing test
  does.
- `sync_meshes` and `sync_lod` resolve `MeshSource{path, id}` through the
  registry before calling `resolve_mesh`. The script sync does the same for
  `Script`.
- Console: `asset.list`, `asset.set <id> <path>`, `asset.save`, `asset.reload`.
- Editor: an Assets panel listing key to path, marking dangling keys, with a
  reload button.

### Tests

`test/asset_registry_test.cpp`: round-trip; resolve prefers the registry over
the path; empty id falls back; unknown id falls back and reports dangling;
malformed file leaves the table intact; names sorted; erase.

`test/scene_asset_test.cpp`: a scene whose `MeshSource` carries a stale path
and a known id resolves geometry from the registry's path.

## Sub-project 2: Textured materials

### The problem

One draw call cannot bind per-material textures. Two options:

- **(a) Batch by material.** Sort vertices by material, record a draw range
  per material, issue one `SDL_DrawGPUPrimitives` per range. N draw calls.
- **(b) Texture array or atlas** plus a per-vertex layer index. Keeps one draw
  call but forces every texture to the same size and format, and needs a fixed
  maximum.

**Decision: (a).** It is the conventional answer, does not constrain texture
size or format, and leaves the Hi-Z pyramid and culling untouched — they
operate on vertices, not on draw calls. The cost is N draw calls instead of 1,
which is the price of texturing and is what every real engine pays.

### Vertex

`GpuVertex` gains `glm::vec2 uv`. Geometry is still pre-transformed, so the
vertex stage stays trivial and the single vertex buffer stays one allocation.

### Material

```cpp
struct Material {
    glm::vec3 base_color{0.82f, 0.82f, 0.86f};
    std::string albedo_id;   // asset key; empty means untextured
    float roughness{0.5f};   // reserved; lit.frag ignores it until PBR
};
```

`base_color` already serializes. `albedo_id` is new and optional, so a v3
document loads unchanged.

### Textures

A `Texture` is an `SDL_GPUTexture` plus an `SDL_GPUSampler`, owned by the
renderer and keyed by asset id. Loaded through `stb_image` (already vendored,
currently unused), uploaded once, cached by id. A missing texture falls back to
a 1x1 white texture so a bad reference never blanks a mesh.

### Shader

`lit.frag` gains `layout(set = 2, binding = 0) uniform sampler2D albedo;` and
multiplies `v_color * texture(albedo, v_uv).rgb`. Set 2 is free and is SDL's
documented home for fragment sampled textures. Untextured materials bind the
white fallback, so there is one shader rather than two.

### GLB

The loader gains TEXCOORD_0 and a minimal material read: `baseColorFactor` and
`baseColorTexture`. An embedded image is written to the assets directory as a
sidecar PNG and keyed in the registry by `<glb name>#<material index>`.

### Risk

This is the largest sub-project and it touches the pipeline the Hi-Z work
depends on. The two-colour-target layout must stay exactly as it is.

## Sub-project 3: Physics (Jolt)

- Vendor Jolt under `dependencies/jolt` as a submodule, built as a static
  library with only the pieces needed — no samples, no renderer.
- `PhysicsWorld` owns a `JPH::PhysicsSystem`, a single-threaded temp
  allocator (matching the engine's single-threaded rule), and the layer
  filters.
- Components: `RigidBody{shape, motion_type, mass, friction, restitution}`,
  `BoxCollider{half_extents}`, `SphereCollider{radius}`.
- `Transform` stays the source of truth for kinematic and static bodies.
  Dynamic bodies write back into `Transform::position` and `Transform::rotation`
  after the step.
- Step order: `Scene::update` then `PhysicsWorld::step(dt)` then write back
  transforms then `update_world_transforms`. The ordering matters — the
  renderer and culling read `Transform::world`, so physics must land before
  them.
- Character controller and raycasts exposed for game code. A physics raycast
  would later replace the bounding-sphere picking the editor uses today.
- Determinism: fixed timestep with an accumulator, capped at N substeps per
  frame so a stall cannot spiral.

## Sub-project 4: Animation (transform/keyframe)

- `AnimationClip` is a set of named tracks. Each track targets an entity GUID
  plus a property (`position`, `rotation`, or `scale`), with keyframes of
  `(time, vec3)` and linear interpolation.
- An `Animator` component holds `{clip_id, time, speed, loop, playing}`.
- `AnimationSystem::update(scene, dt)` advances each animator and writes the
  target's `Transform`. It runs **before** physics so an animated platform can
  carry a body.
- Clips are assets, keyed through the registry. Format: JSON alongside the
  scenes, one file per clip, the same shape prefabs already use.
- `Animator` serializes; the clips live in files.
- No skinning, no blending, no events. Those are out of scope until the
  material restructure has landed and skeletal animation is designed on top of
  it.

## Sub-project 5: Wiring the orphaned libraries

Each of these is implemented, tested, and called by nothing. The decision per
item is wire or delete:

| Item | Decision |
|---|---|
| `AsyncSceneLoad` | **Wire.** `scene.load_async <name>` console command; the frame loop polls and adopts on completion. Removes the stall on large documents. |
| `ResidencyPolicy` | **Wire.** A `ResidencySystem` in the frame loop that loads and unloads regions from the active scene's origins. The frame-budgeted scheduler stays deferred; this is the unbudgeted version the composition spec already describes. |
| `HeapAllocator`, `PoolAllocator` | **Test, then leave.** Add the two test files their own spec mandates, then leave them as a library surface. Do not force consumers. |

## Build and verification

- Build: `cd build && cmake --build . -j"$(nproc)"`.
- Tests: `build/bin/lotus_tests`. Baseline is **200 passing**; never lower.
- No CMake edits for new files under `src/` or `test/`. Jolt is the exception:
  it needs an `add_subdirectory`.

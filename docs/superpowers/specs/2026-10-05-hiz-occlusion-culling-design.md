# Hi-Z occlusion culling

## Goal

Cull entities hidden behind other geometry, so the CPU stops transforming their vertices and the GPU stops shading fragments nothing can see.

Distance-based LOD and frustum culling already exist (`src/scene/lod.cpp`, `src/scene/culling.cpp`) and are out of scope. This adds a third, independent rejection stage that runs after both.

## Repo constraints

These are facts about the existing code, not preferences. Each one shapes the design.

- Geometry is uploaded already transformed to world space and drawn in a single batch (`shaders/lit.vert`, `Scene::draw_calls`). Culling must therefore happen on the CPU before `emit_mesh`, never as a per-draw GPU decision.
- `Scene::build_vertices` is pure CPU on purpose so culling stays testable (`include/lotus/scene/scene.hpp`). The occlusion test stays in plain C++; only the pyramid reduction is a shader.
- `SDL_GenerateMipmapsForGPUTexture` averages its mip chain. Averaging blurs occluder boundaries and culls objects that are partly visible. It is not used. Reduction is `min`.
- `SDL_GPUShaderStage` has no `SDL_GPU_SHADERSTAGE_COMPUTE`. Compute pipelines are built from `SDL_GPUComputePipelineCreateInfo` (raw SPIR-V, entrypoint, threadgroup size) instead.
- `shaders/build-shaders.sh` loops `vert frag` only. It is extended to cover a `comp` stage.
- The UI layer draws after `end_scene_pass`, so ImGui never writes the depth target and cannot pollute the pyramid.
- Offscreen mode (`init_offscreen`, `capture_rgba`) reads color target mip 0. Adding a second color target does not change what `capture_rgba` returns.

## Rules

- Occlusion never blocks. A frame whose fence is not ready skips occlusion and reports `occlusion_ready() == false`.
- Occlusion never throws. A pipeline or format failure disables the feature and logs once; the engine keeps running.
- The test is one-directional. Culling something visible is a bug. Drawing something hidden is a cost, not a bug.
- An empty, missing, or resized pyramid treats every entity as visible.
- `Scene::build_vertices` keeps its behaviour and signature when passed no pyramid. Existing tests compile and pass unchanged.
- Occlusion is derived per frame. It is never serialized.
- The depth target is cleared to a far value every frame. Never rely on unwritten texels; `min` reduction turns an unwritten texel into a scene-wide false occlusion.
- `init_offscreen` creates and sizes the pyramid texture exactly as the windowed path does. Offscreen capture is a supported mode, not a fallback.

## Depth source

The scene pass gains a second color target and `lit.frag` writes linear view depth alongside colour. No separate prepass, no extra draw, no new graphics pipeline.

`lit.vert` gains a `mat4 view` in `VertexUniforms` (currently `view_projection` alone) and outputs the view-space position. `lit.frag` writes `-view_position.z` to location 1.

The value is linear view depth rather than `gl_FragCoord.z`. Both are monotonic, so `min` reduction is valid for either, but linear depth removes the perspective divide from the CPU-side comparison and keeps the pyramid in world units.

The target is `SDL_GPU_TEXTUREFORMAT_R32_FLOAT`.

The scene pass clears color target 1 to `(1e9, 1e9, 1e9, 1e9)` every frame, before any draw. This is load-bearing, not cosmetic. An unwritten texel is not neutral: it reads as roughly `0`, which `min` reduction propagates across the entire pyramid, so every entity compares as behind it and is culled. That is the false-positive failure the Rules section forbids, and it would be total rather than partial. A far clear makes untouched texels mean "nothing here".

`1e9` is well inside the `R32_FLOAT` range and far beyond any camera far plane, so it never collides with real geometry depth.

## Pyramid

`shaders/depth.comp` reduces each mip from the one above it with `min()`. It runs as **one compute pass per level**, each opened by `SDL_BeginGPUComputePass` with exactly two `SDL_GPUStorageTextureReadWriteBinding` entries: slot 0 on mip `L-1`, slot 1 on mip `L`. Both are declared `uniform image2D` in GLSL; the shader reads slot 0 and writes slot 1, and the two addresses never overlap within a dispatch.

This shape is forced by the API. `SDL_BindGPUComputeStorageTextures` takes bare `SDL_GPUTexture *const *` with no mip level (SDL_gpu.h:3712), and `SDL_GPUTextureSamplerBinding` carries none either (SDL_gpu.h:2142). `SDL_GPUStorageTextureReadWriteBinding` is the only binding with a `mip_level` field (SDL_gpu.h:2174), and it is fixed when the pass begins, so bindings cannot be swapped mid-pass. One pass per level is therefore the only way to address a specific mip.

The pyramid texture is `R32_FLOAT` with mip count `floor(log2(min(width, height)))`, flagged `SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE`. Both compute slots are read-write bindings, so the simultaneous flag is required and is explicitly not equivalent to `READ | WRITE` (SDL_gpu.h:912). This SDL version has no `SDL_GPU_TEXTUREUSAGE_COPY_SRC`; copy usage is inferred by the backend.

Mip 0 is full resolution. It is the render target the scene pass writes; it is never transferred to the CPU.

`ensure_depth_target` grows or recreates the pyramid alongside the existing depth buffer. Offscreen mode sizes it too.

## Readback and latency

`end_frame` submits the single per-frame command buffer with `SDL_SubmitGPUCommandBufferAndAcquireFence` (`src/core/application/application.cpp:374-390` already runs exactly one command buffer per frame, so no extra synchronisation is needed). After the scene pass, and before that submit, a copy pass downloads mips `K..N` (`K = 2`, quarter resolution and coarser — about 675 KB total at 1080p) into a transfer buffer.

The compute pass and the copy pass are sequential on that one command buffer: `SDL_EndGPUComputePass` runs first, then `SDL_BeginGPUCopyPass`. `SDL_DownloadFromGPUTexture` takes a `SDL_GPUCopyPass*`, so a copy pass cannot overlap an open compute pass. Mips are downloaded in ascending order.

Per-mip addressing is explicit. `SDL_GPUTextureRegion` carries its own `mip_level` (SDL_gpu.h:1469), but SDL does not pack the mip chain, so each download computes its own `SDL_GPUTextureTransferInfo`: `offset` is the running byte total and `rows_per_layer` is the mip's own height. Rows are padded to a 256-byte pitch, so `pixels_per_row` is that padded pitch expressed in pixels, not the raw width — it is the destination row stride, and setting it to the raw width makes the GPU write row $y$ at $y \cdot w \cdot 4$ while the CPU unpack walks $y \cdot \text{pitch}$, so every row after the first is garbage. Leaving the field zero is worse still: it falls back to the region size (SDL_gpu.h:1395-1399). The padding keeps SDL from inserting a temporary repacking copy on D3D12 (SDL_gpu.h:1401-1405).

At the start of frame N+1 the fence is polled with `SDL_QueryGPUFence`, never waited on. Ready maps the buffer with `SDL_MapGPUTransferBuffer` and replaces the CPU-side pyramid. Not ready skips occlusion for that frame.

The CPU therefore tests against the pyramid built one or more frames ago. This is the intended trade: staleness costs a little performance, and a blocking readback would serialise CPU against GPU.

`begin_frame` polls the previous frame's fence with `SDL_QueryGPUFence` and never waits on it, so the loop stays stall-free while single-threaded.

## The test

`DepthPyramid` is a standalone class in its own translation unit. It does not include `SDL_gpu.h` and needs no GPU.

```cpp
bool occluded(const glm::mat4& view, const glm::mat4& view_projection,
              const BoundingSphere& world_sphere) const;
```

Steps:

1. Transform the sphere centre by `view`. Reject when `center_view.z > -radius`, since the sphere straddles the near plane.
2. Project the eight corners of the sphere's view-space AABB, divide by `w`, and take the screen-space min and max as the sphere's pixel AABB. Reject when any corner has `w <= 0`.
3. Reject when the AABB does not overlap the viewport.
4. Inflate the radius by 2% before this point, so an entity does not occlude itself and flicker as the camera moves.
5. Pick the level as `clamp(ceil(log2(max(aabb_width_texels, aabb_height_texels))), K, N)`, the coarsest mip that still contains the AABB in at least one texel.
6. Return true only when the sphere's nearest linear depth, `-center_view.z - radius`, is at least the maximum depth over the selected mip region.

Step 6 in isolation:

$$\min d_{\text{sphere}} \;\ge\; \max_{\text{mip}} d_{\text{occluder}} \;\Rightarrow\; \text{occluded}$$

Projection uses the same zero-to-one depth convention as `Frustum::from_view_projection`, since both consume the same `Camera3D::view_projection`.

## Integration

| File | Change |
|---|---|
| `include/lotus/scene/depth_pyramid.hpp` | New. `DepthPyramid`, GPU-free. |
| `src/scene/depth_pyramid.cpp` | New. Construction and the test. |
| `shaders/depth.comp` | New. `min` reduction. |
| `shaders/lit.vert`, `shaders/lit.frag` | Add `view` matrix; emit linear depth as color target 1. |
| `shaders/build-shaders.sh` | Compile the `comp` stage. |
| `include/lotus/gpu/renderer.hpp`, `src/gpu/renderer.cpp` | Pyramid texture, transfer buffer, compute pipeline, fence. `occlusion_pyramid()`, `occlusion_ready()`, `occlusion_enabled()`. |
| `include/lotus/scene/scene.hpp`, `src/scene/scene.cpp` | `build_vertices(aspect, const DepthPyramid* = nullptr)`, `occluded_entities()`. |
| `src/ui/editor.cpp` | Occluded count on the existing stats line. |
| `app/main.cpp` | A wall in front of the LOD sphere so the effect is visible. |

`Scene::upload` reads `gpu.occlusion_pyramid()` and forwards it. The occlusion check runs inside `build_vertices` after the frustum test, so `culled_entities` still counts every rejection while `occluded_entities` counts only the new stage.

## Failure handling

| Condition | Behaviour |
|---|---|
| Compute pipeline or storage format unsupported | `occlusion_enabled() == false`, one log line, frustum and LOD culling unaffected |
| Fence not ready at frame start | Skip occlusion, `occlusion_ready() == false` |
| Window or offscreen target resized | Pyramid invalidated, `occlusion_ready() == false` until rebuilt |
| `DepthPyramid` empty | Every entity visible |
| `build_vertices` called with `nullptr` | Behaviour identical to today |
| Scene pass begins without clearing target 1 | Not reachable: the clear is part of `begin_scene_pass`, so no path can skip it |

## Tests

`test/occlusion_test.cpp`, all CPU-only:

- An empty pyramid never occludes, whatever the sphere.
- A sphere entirely behind an occluder is occluded.
- A sphere straddling an occluder's edge stays visible.
- A sphere overlapping the occluder's front face stays visible.
- A sphere spanning several mip levels still classifies correctly at the coarsest level it selects.
- A sphere partly behind the near plane is never occluded.
- A sphere outside the viewport in the X or Y axis is never occluded.
- Linear depth and the projection follow the zero-to-one convention `Frustum` uses, matching `Camera3D`.
- `min` reduction over a known depth image produces the expected mip chain.

Scene-level, in the same file:

- Feeding a synthetic pyramid through `build_vertices` removes an occluded entity from the vertex buffer, increments `occluded_entities`, and still counts it in `culled_entities`.
- Passing `nullptr` reproduces current vertex output exactly.

GPU-side, verified through `LOTUS_MAX_FRAMES` (`src/core/application/application.cpp:335`) running the offscreen path headless for enough frames that the fence becomes ready:

- The pyramid round-trips: after the first few frames, `occlusion_ready()` is true and a wall placed in front of the LOD sphere reports it occluded while `lod_sphere` stays visible.
- The engine never blocks: a run completes in wall-clock time consistent with its frame count.
- Resizing mid-run invalidates the pyramid and then rebuilds it, with no crash and no false occlusion in between.

## Non-goals

- Per-entity occluder flags. Everything drawn writes depth.
- Serialization. Occlusion is derived state.
- Any change to LOD selection.
- GPU-side sphere testing; the test stays in C++ so it is unit-testable.
- Latency beyond one frame, and no occlusion query bookkeeping.

## Build

No CMake change is needed. `CMakeLists.txt` globs `src/*.cpp` into `LOTUS_SOURCES` and `test/*.cpp` into `LOTUS_TEST_SOURCES`, both with `CONFIGURE_DEPENDS`, so `src/scene/depth_pyramid.cpp` and `test/occlusion_test.cpp` are picked up automatically.

`depth.comp` must be regenerated before the first build. `glslangValidator` compiles a `.comp` file with `-V` and the same default entrypoint `main`, matching the existing `vert`/`frag` handling.

Three shaders need regenerating, not one: `depth.comp` is new, and `lit.vert` and `lit.frag` both change. Their committed SPIR-V headers cannot be patched by hand, so a shader compiler is a hard prerequisite for this feature on every approach, not just the compute one.

`glslangValidator` is not on `PATH` in this environment and `dnf install` needs root. The script's own comment documents the workaround, which works:

```
dnf repoquery --location glslang
dnf download --destdir=/tmp/glslang-rpm glslang-16.2.0-1.fc44.x86_64
rpm2cpio /tmp/glslang-rpm/glslang-*.rpm | cpio -idm -D /tmp/shadertools
GLSLANG=/tmp/shadertools/usr/bin/glslangValidator ./shaders/build-shaders.sh
```

That extracted glslang 16.2.0 reproduces the committed `lit.vert.spv.h` (1508 bytes) and `lit.frag.spv.h` (2428 bytes) exactly, so it matches the version that generated them. `.comp` files compile with the same `-V` flag and default `main` entrypoint as `vert` and `frag`.

`build-shaders.sh` is extended to loop `vert frag comp`, writing `depth.comp.spv.h` next to the existing generated headers.

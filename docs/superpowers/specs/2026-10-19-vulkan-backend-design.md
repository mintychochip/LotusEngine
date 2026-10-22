# Vulkan backend for `GpuRenderer` — design

Status: approved by the user on 2026-10-19 (staged full replacement, Vulkan 1.3 core).
Branch: `vulkan-backend` (worktree `../LotusEngine-vulkan`), based on `9a85f69`.
Stage 2 (W1-W3) landed 2026-10-21: the windowed path is complete and SDL_GPU is removed from the tree
(last present at `adb7a77`; `git show adb7a77:src/gpu/sdlgpu/renderer_sdlgpu.cpp` is the old
behavioural reference).
Merged with `master` (`84a9497`) on 2026-10-21; its rendering additions were ported to this backend in the
merge (see "Merge with master").

## Goal

Replace SDL3's GPU API (SDL_gpu) inside `GpuRenderer` with a direct Vulkan 1.3
implementation that draws **the same pixels** and is **measurably faster**, and
quantify the gain by A/B against the SDL_GPU build.

Acceptance (all must hold):

1. Bench frame hashes identical to the SDL_GPU build's, built from the same source (`master` `84a9497`, out of
   tree): NVIDIA `83a9de542ad33701`, Intel ARL `a3bfbe901faae371`, lavapipe `a467d4ea5cdd618d`
   (`lvp_image_exact=1` against the regenerated golden); `lotus --headless` screenshots and state dumps of
   `main`, `jungle`, `east_wing` and `hub` at 1 and 120 ticks byte-identical to the SDL_GPU build on all three
   devices. (Before the merge the hashes were `a593920ea4013023`, `ce325cdd979df2a7`, `b380d9a84f4bde8f`.)
2. Whole suite (376 tests) passes on RTX 3080, Intel ARL and lavapipe (NVIDIA and lavapipe skip the two
   windowed swapchain tests: no headless surface there), and the Khronos validation layer reports **zero**
   errors/warnings on all of them.
3. Window path: runs on the user's Wayland session, and the swapchain path is
   exercised headlessly (`SDL_VIDEO_DRIVER=offscreen` gives a `VK_EXT_headless_surface`
   that Intel can present to; NVIDIA cannot, and neither can lavapipe: Mesa 26.0.3's
   headless WSI always builds dma-buf images while its common code leaves
   `vkGetMemoryFdKHR` unloaded on software devices, so `vkCreateSwapchainKHR` calls a
   null pointer inside the driver). NVIDIA and lavapipe are covered by running the
   windowed application on the Wayland surface instead (lavapipe presents there
   through wl_shm), validation and synchronization validation on.
4. A/B report: sync frame (`frame_ms`, `cpu_ms`, `gpu_ms`) and pipelined throughput
   (fps, Mverts/s) on the three devices, SDL_GPU vs Vulkan, interleaved.

Non-goals: new rendering features, changed shader math, async compute / multi-queue,
bindless, GPU-driven culling, Vulkan < 1.3, mobile tiling optimisations.

## Backends and compatibility (decided 2026-10-19, after the first draft)

The engine targets three graphics APIs, each a backend behind `GpuRenderer`'s unchanged
public API, selected at configure time (`LOTUS_GPU_BACKEND`):

| backend | platforms | status |
|---|---|---|
| `vulkan` (this spec) | Linux, Windows | built and verified here |
| `d3d12` (hand-written) | Windows 10+ | written here; **unverified** — no Windows runtime on this machine |
| `d3d11` (hand-written) | Windows 7/8, pre-2015 Intel GPUs | same |

SDL_GPU is **not** retained as a backend: its D3D12 path is replaced by the hand-written
backends, and the clean cutover stands. Consequences, named to the user and accepted:
Windows has no working backend until `d3d12`/`d3d11` land, and macOS loses its backend
(SDL's Metal path) unless a Metal backend is written later. D3D11 is required because it
is the only API that reaches Windows 7/8 and pre-2015 Intel GPUs; SDL has no D3D11 GPU
backend at all, so it is a from-scratch renderer either way.

What "unverified" means, precisely: the D3D backends **compile** (checked with the
MinGW-w64 cross toolchain, `mingw64-gcc-c++`/`mingw64-headers` extracted under
`/tmp/mingw` — proven: C++23 with `d3d12.h`, `d3d11.h`, `dxgi1_6.h`), and their HLSL
**compiles to DXBC** (checked with `vkd3d-compiler` 1.17: `-x hlsl -p vs_5_1 -b dxbc-tpf`
— proven to emit a `DXBC` container). Nothing about their runtime behaviour, image
equality or performance is claimed. Every D3D source file carries a header comment
saying so, and the README repeats it in the backend matrix.

D3D shaders: the GLSL stages are re-expressed in HLSL (same math, same uniform layouts,
a `cbuffer` per block laid out like the std140 ones), compiled to DXBC SM 5.1 blobs by
`shaders/build-shaders-d3d.sh` (vkd3d-compiler on Linux; fxc/dxc on Windows) and
committed as headers, exactly like the SPIR-V ones.

### The seam

`include/lotus/gpu/renderer.hpp` declares an abstract `GpuRenderer`: the API only, no backend types.
`create_gpu_renderer()` returns the renderer the build links. Each backend is a concrete class in its
own directory (`src/gpu/vulkan/`, later `src/gpu/d3d12/` and `src/gpu/d3d11/`) implementing that
interface; nothing else in the engine names a backend type, and every caller holds a `GpuRenderer&`
from the factory. While SDL_GPU and Vulkan coexisted, a `LOTUS_GPU_BACKEND` CMake variable
(`sdlgpu|vulkan`) chose between them, defaulting to `sdlgpu` until the Vulkan backend passed per-frame
verification. SDL_GPU was removed only after that (the user's explicit sequencing), and the variable
with it: with one backend it selected nothing. The first Direct3D backend reintroduces a selector.

### Verification: per frame

A port is proven on every frame, not only on the last one. `lotus_render_bench --frame-hashes N` renders
the standard camera loop synchronously, captures each frame and prints `FRAMEHASH <index> <hash>`;
`ab_frames.sh N A B` diffs two builds' sequences and reports the first divergence. History-dependent
state — the Hi-Z pyramid's one-frame lag, SSAO's one-frame lag, frames in flight — breaks specific frames
and nothing else, so this is the acceptance test between SDL_GPU and Vulkan on each device. Timing keeps
its own per-frame distribution (`frame_ms`, `frame_ms_p90`, per-round medians).

### Tests on machines without a Vulkan device

The GPU tests skip, with the reason, when the Vulkan loader reports no device the renderer could use
(`test/vulkan_device.hpp`: SDL cannot load Vulkan, no instance, no physical device, none at Vulkan 1.3). The
probe asks the loader directly instead of catching the renderer's exception, because a device that exists but
fails to initialise must still fail the tests; the windowed tests also skip when the loader lacks
`VK_EXT_headless_surface` (NVIDIA). `LOTUS_REQUIRE_VULKAN=1` turns a missing device into a failure, which CI
sets: a probe that wrongly claimed no device would otherwise drop the GPU tests silently (checked by mutation).
`ctest` runs the suite a second time with the drivers hidden (`LotusTestsWithoutVulkanDevice`), so a Vulkan
test that forgets the check is caught on machines that have a GPU.

## What SDL_GPU does today (so the port can match it)

SDL's Vulkan backend (`SDL_gpu_vulkan.c`, SDL 3.4.0) is Vulkan 1.0 core + `KHR_maintenance1`:
VkRenderPass+framebuffer cache, per-subresource barriers, one 32 KB uniform pool per command
buffer with dynamic offsets, `depthClampEnable = !enable_depth_clip` (so **depth is clamped**),
negative-height viewport `{y=h, height=-h}`, DOWNLOAD buffers `HOST_VISIBLE|COHERENT` (not
guaranteed cached), swapchain `B8G8R8A8_UNORM` (SDR), 3 frames in flight. Everything the engine
sets is recorded in the scout reports summarised in the plan; the rendering state to reproduce is:

- rasteriser: fill, cull NONE, front face CCW, no depth bias, depth clamp ON, 1 sample, TRIANGLE_LIST
- depth: D32_FLOAT `LESS_OR_EQUAL` (lit, sky no-write); shadow D16_UNORM; geometry viewport depth
  range `[0, 0.5]`, sky at `1.0`
- blend off; sky pipeline masks colour attachment 1 (`colorWriteMask = 0`, needs `independentBlend`)
- vertex input: slot0 `GpuVertex` 48 B, slot1 uv 8 B, slot2 `GpuInstance` 96 B (instance rate; 80 B before the merge)
- samplers: default LINEAR/LINEAR/mip LINEAR/REPEAT; shadow NEAREST/CLAMP; fxaa LINEAR/CLAMP;
  ssao depth NEAREST/CLAMP; no anisotropy/compare/lod bias
- textures: albedo `R8G8B8A8_UNORM` with full mip chain generated by level-by-level LINEAR blits;
  pyramid `R32_SFLOAT` 3 mips; ssao `R8G8B8A8_UNORM` quarter-res; shadow 2048² D16 (1024² before the merge)

## Architecture

SDL keeps window, events, input and the Vulkan loader (`SDL_Vulkan_LoadLibrary`,
`SDL_Vulkan_GetVkGetInstanceProcAddr`, `SDL_Vulkan_CreateSurface`). No link-time `libvulkan`, no
Vulkan SDK. Headers: system `vulkan/vulkan.h` if present, else SDL's bundled
`dependencies/SDL/src/video/khronos` (VK_HEADER_VERSION 282).

```
src/gpu/vk/
  vk_api.{hpp,cpp}        PFN table (global/instance/device), VkResult -> text, VK_CHECK
  vk_context.{hpp,cpp}    instance (+validation, debug messenger), physical-device pick,
                          device (1.3: dynamicRendering, synchronization2; 1.0: depthClamp,
                          independentBlend), queue, one-shot submit
  vk_memory.{hpp,cpp}     block sub-allocator (DeviceLocal / Dynamic / Staging / Readback),
                          buffers + optimal images, mapped pointers, flush/invalidate
  vk_barrier.{hpp,cpp}    per-subresource layout/stage/access tracking -> sync2 barriers
src/gpu/
  renderer.cpp            init/shutdown, frame ring, begin/end frame, wait_idle
  renderer_geometry.cpp   mesh cache, dynamic instance/immediate streams, uploads, draw recording
  renderer_passes.cpp     shadow / lit / sky / fxaa / ssao pipelines + pass recording
  renderer_occlusion.cpp  Hi-Z compute, readback, adopt
  renderer_textures.cpp   targets, samplers, albedo cache, capture
  renderer_swapchain.cpp  surface, swapchain, acquire/present (stage 2)
include/lotus/gpu/renderer.hpp   public API only; Vulkan is private (pimpl)
```

`GpuRenderer` keeps its public API except the SDL handles. **Removed:** `device()`,
`command_buffer()`, `swapchain_texture()`, `swapchain_format()`, `viewport_texture()`.
**Added:** `wait_idle()`, `device_name()`, `capture_viewport_rgba()`, UI hooks
(`init_ui_backend()`, `shutdown_ui_backend()`, `ui_backend_new_frame()`, `render_ui(ImDrawData*)`,
`viewport_ui_texture()`), so the editor and tests stop touching GPU handles.
Offscreen frames never queue more than `kFramesInFlight` (2): `begin_frame_offscreen()` waits for the
oldest.

### Frame ring and lifetime

Two frame slots. Each owns: command pool + primary buffer, fence, **dynamic buffers** (instance
records, immediate vertices/uvs, UBO ring — persistently mapped), a transient descriptor pool, a
Hi-Z readback buffer, and a **garbage list**. A resource released at any time goes to the current
slot's garbage list and is destroyed when that slot's fence has been waited on (frames complete in
queue order, so every earlier use has finished). No `vkDeviceWaitIdle` except in `wait_idle()`,
`resize` of targets that are still in flight, and `shutdown()`.

### Memory

- `Dynamic`: `DEVICE_LOCAL|HOST_VISIBLE|COHERENT` when the device has it (3080: ReBAR heap, Intel/
  lavapipe: UMA), else `HOST_VISIBLE|COHERENT`. The CPU writes instance records straight into it:
  **no staging copy and no copy pass** (this is what `begin_geometry_upload` already promises).
- Static meshes: one `DeviceLocal` buffer per mesh `[vertices | uvs | indices]`, uploaded through a
  staging buffer in the frame's command buffer with one barrier.
- `Readback`: prefers `HOST_CACHED` (Intel, lavapipe, 3080 host heap) with `vkInvalidateMappedMemoryRanges`;
  only when no cached type exists does the existing MOVNTDQA streaming-load copy apply.

### Descriptors (the first port keeps the shaders' existing set numbers)

The committed SPIR-V declares its uniforms at the set numbers SDL_GPU's Vulkan backend imposes
(set 1 vertex uniforms, set 2 fragment samplers, set 3 fragment uniforms; compute uses set 1 for
read-write storage images). **The first Vulkan port keeps those numbers**, so the shader headers
stay byte-identical to the ones the SDL build rendered with and image identity is easier to argue.
The layout below is the *later* re-layout, measured as its own Phase 3 optimisation once the port
works:

| set | content | lifetime |
|---|---|---|
| 0 (pass) | b0 UBO dynamic (vertex), b1 UBO dynamic (fragment), b2 sampler, b3 sampler | per-slot transient pool, one set per pass |
| 1 (material) | b0 albedo (lit only) | one persistent set per albedo texture |
| compute set 0 | b0 `r32f` src, b1 `r32f` dst | persistent per Hi-Z level pair |

lit: b0/b1 UBOs, b2 shadow, b3 ssao; shadow: b0; sky: b0, b1; fxaa: b1, b2 source; ssao: b1, b2 depth.
No push constants. Draws bind a material set only when the albedo or the normal map changes. Until the
re-layout, the port binds three sets per pass (vertex uniforms / four samplers: albedo, shadow, SSAO, normal
map / fragment uniforms) and caches one descriptor set per sampler combination for the sampler set. The cache
pool is created with `FREE_DESCRIPTOR_SET_BIT`: when images are replaced (a resize) the sets that named them go
back to it with the other retired targets, once the frames in flight are done with them.

### Passes (dynamic rendering, sync2)

Barrier plan per frame: shadow image → DEPTH_ATTACHMENT → (after) READ_ONLY for the fragment stage;
lit targets (colour, pyramid mip 0, depth) → attachment layouts; fxaa source → shader read; pyramid
mip 0 → shader read (SSAO) → GENERAL (compute) ; mip 2 → TRANSFER_SRC → copy to readback; ssao → colour
attachment → shader read (consumed next frame). Barriers synchronise across command buffers in
queue order, so single instances of the shared images serve both frames in flight.

### Windowed path (stage 2)

`SDL_Vulkan_CreateSurface`; `B8G8R8A8_UNORM`/`R8G8B8A8_UNORM` only (the shaders sRGB-encode);
present mode FIFO by default, `LOTUS_PRESENT_MODE=mailbox|immediate` for uncapped fps;
`OUT_OF_DATE/SUBOPTIMAL` recreate; images = `max(3, minImageCount)`. ImGui: `imgui_impl_vulkan`
with dynamic rendering, owned by the renderer behind the UI hooks. `LOTUS_VK_VALIDATION=1` enables
the validation layer + debug messenger (errors counted and printed); `=sync` adds synchronization
validation, which the windowed test runs under.

Swapchain synchronisation (each point is a validation finding, not a style choice):

- The acquire semaphore is per frame slot and the slot's fence is waited **before** the acquire; the
  slot is only taken (fence reset) once the image is in hand, so a skipped acquire cannot strand the
  fence unsignalled. A single shared acquire semaphore is pending-in-use at the next acquire
  (VUID-vkAcquireNextImageKHR-semaphore-01779).
- The present (render-finished) semaphore is per swapchain **image**, indexed by the acquired image:
  a present's wait is only known complete once its image is re-acquired
  (VUID-vkQueueSubmit2-semaphore-03868).
- The submit waits on the acquire at `COLOR_ATTACHMENT_OUTPUT`, and the image's tracked state starts at
  that stage after every acquire, so its first layout transition names it as its source; otherwise the
  transition is a WRITE_AFTER_READ against the acquire.
- A rebuild (resize, OUT_OF_DATE, SUBOPTIMAL) idles the device first: frames in flight still reference
  the old images, views and present semaphores, and a resize keeps the format, so the pipeline rebuild
  cannot be relied on to wait.
- OUT_OF_DATE at acquire acquires nothing: the frame is skipped and the rebuild happens in the next
  `begin_frame`, at the window's current size (rebuilding inside the acquire would use the stale stored
  size, and a second consecutive OUT_OF_DATE would have no way out). A surface reporting 0x0 (a
  minimized window) is a quiet skip, not a failure. A first-ever creation failure is fatal, as in
  SDL_GPU (a window that runs but never draws is no fallback); a failed rebuild skips frames and says
  so once.
- The session's colour format is the swapchain's (B8G8R8A8 on every driver that can present here), so
  the viewport follows it and `capture_viewport_rgba()` swizzles to the RGBA8 the API promises. A
  viewport pass that draws nothing still begins rendering into the viewport's own targets
  (`end_viewport_pass` closes the pass only after that).
- ImGui resolves its entry points through `vkGetInstanceProcAddr` (it asks for instance- and
  device-level names alike; `vkGetDeviceProcAddr` is invalid for the former).

## Method

1. **Prep on the SDL_GPU backend** (so A and B run identical callers): `wait_idle()`, `device_name()`,
   UI hooks, `capture_viewport_rgba()`, offscreen frame throttle, bench `--pipelined`. Freeze that build as A.
2. **Stage 1** — Vulkan backend for the offscreen path; ported tests and bench; validation clean; hashes
   identical; A/B checkpoint.
3. **Stage 2** — swapchain, ImGui, `LOTUS_PRESENT_MODE`; remove SDL_GPU, `imgui_impl_sdlgpu3`; docs.
4. **Vulkan-only optimisations**, each A/B'd against the faithful port (fused 4x4 Hi-Z dispatch,
   skipping the linear-depth clear when the sky covers it, descriptor caching, …).
5. **D3D12 backend** (hand-written, Windows): the same pass structure behind the same seam;
   compile-checked with MinGW-w64, shaders via vkd3d-compiler; labelled unverified.
6. **D3D11 backend** (hand-written, Windows 7/8): the same feature set with D3D11's state
   mapping (input layouts, constant buffers, no explicit barriers, and the Hi-Z reduction
   degrading where compute is unavailable, exactly as the SDL path's occlusion flag does).

A/B protocol: frozen binaries, ABBA interleaved, `taskset` P-cores, check `uptime`, n ≥ 20 on the
3080. Sync mode = the existing bench (median of frames). Pipelined mode = 2 frames in flight, occlusion
on, N seconds, fps + Mverts/s + mean drawn entities. "vps" is read as fps; Mverts/s is reported too.

## Merge with master (2026-10-21)

`master` (`84a9497`) kept changing the SDL_GPU renderer while this branch removed it: normal maps, metallic and
emissive materials, up to eight point lights, analytic environment specular, a 2048² shadow map with a 5×5 PCF
and a 20-unit ortho half-extent (15 before), `GpuInstance` 80 → 96 B, `FragmentUniforms` 368 → 656 B, and a
fourth sampler at set 2 binding 3. The merge took this branch's deletion of `src/gpu/renderer.cpp` and ported
those changes to `GpuRendererVulkan`; the abstract `GpuRenderer` gained `set_point_lights()` and
`LightingSettings::environment_intensity`. The SPIR-V headers are master's and regenerate byte-identically from
the merged GLSL with glslang 16.2.0.

Verified against master's SDL_GPU renderer built out of tree from the same commit (master's own `build/` was
stale, so it is no oracle):

- `lotus --headless` screenshot and state dump byte-identical on NVIDIA, Intel and lavapipe for `main`, `jungle`
  (5 point lights, 5,357 normal-map references, emissive and metallic materials, `environment_intensity` 0.7),
  `east_wing` and `hub`, at 1 and 120 ticks: 24 of 24 combinations.
- Bench frame hash identical on all three devices (`83a9de542ad33701`, `a3bfbe901faae371`, `a467d4ea5cdd618d`)
  with `vertices=189132`, `drawn_entities=392`, `draw_calls=72` as before. The bench scene has no normal maps or
  point lights, which is why `jungle` is the oracle for them.
- `bench/golden/render_bench.png` was stale on master (the shading change moves the frame by MAE 0.19, max 29), so
  it was regenerated on lavapipe: `image_exact=1` for both builds.
- The suite on all three devices with synchronization validation: zero validation messages.

What the merge turned up: the pool for cached sampler/storage descriptor sets never took sets back, so every
viewport resize (the editor resizes on every pixel of a drag) leaked the sets of that generation. The 2048-set
pool ran out after 681 (NVIDIA) and 684 (Intel) resizes of a one-cube scene: `vkAllocateDescriptorSets` returned
`VK_ERROR_OUT_OF_POOL_MEMORY` and the frame aborted. Lavapipe and the validation layer do not notice.
`clear_descriptor_caches()` now retires the sets with the other retired targets and the pool frees them.
`VulkanRendererSmoke.ViewportResizesReturnDescriptorSetsToThePool` resizes 1,200 times and bounds the number of
live keyed sets, which catches the leak on lavapipe too; with the return disabled it fails on all three devices
(checked by mutation).

## Results: SDL_GPU against Vulkan

Measured before the merge, on the shading both builds then shared (the merge adds the same shader work to both
sides). Frozen binaries, interleaved ABBA, pinned to the eight P-cores, n = 20 / 14 / 12 runs per side (NVIDIA /
Intel / lavapipe), identical image hash on every run. The machine was not quiet: other processes used 66-69 %
of the pinned cores on average (range 45-92 %) during the NVIDIA and Intel runs and 31-44 % (6-71 %) during
lavapipe's, so absolute numbers are inflated and the p-values are only as good as the interleaving.

Sync mode, one frame at a time. The workload is the same on both sides: 189,132 vertices, 392 drawn entities,
72 draw calls per frame.

| device | cpu_ms SDL → VK | record_ms | frame_ms | gpu_ms |
|---|---|---|---|---|
| NVIDIA | 0.155 → 0.137 (−11.8 %, p < 0.001) | 0.029 → 0.019 (−34.9 %, p < 0.001) | 0.450 → 0.425 (−5.6 %, p = 0.20) | 0.269 → 0.265 (−1.5 %, p = 0.75) |
| Intel | 0.269 → 0.235 (−12.8 %, p = 0.03) | 0.078 → 0.065 (−17.8 %, p = 0.02) | 1.80 → 1.72 (−4.5 %, p = 0.30) | 1.53 → 1.48 (−3.3 %, p = 0.33) |
| lavapipe | 0.295 → 0.242 (−18.0 %, p < 0.001) | 0.061 → 0.030 (−51.0 %, p < 0.001) | 20.97 → 17.01 (−18.9 %, p = 0.07) | 20.68 → 16.77 (−18.9 %, p = 0.07) |

Pipelined mode, frames back to back for 3 s. The culling reads the previous frame's Hi-Z, whose age depends on
timing, so the two builds do not draw the same set here:

| device | fps SDL → VK | change (p) | per-block range | drawn entities / frame SDL, VK | vertices / frame SDL, VK |
|---|---|---|---|---|---|
| NVIDIA | 3,437 → 4,011 | +16.7 % (p < 0.001) | +6.5 … +29.4 % | 513, 545 | 263 k, 285 k |
| Intel | 656 → 699 | +6.5 % (p = 0.004) | −2.2 … +17.1 % | 513, 550 | 264 k, 288 k |
| lavapipe | 38.6 → 35.7 | −7.4 % (p = 0.64) | −23.7 … +11.3 % | 513, 550 | 263 k, 288 k |

Reading it:

- Only the CPU-side figures are clearly significant on every device: `cpu_ms` −12 % to −18 % and `record_ms`
  −18 % to −51 %. The sync `frame_ms` gains (4-6 % on NVIDIA and Intel, p = 0.20 and 0.30) are not significant:
  at this size the frame is GPU-bound and `gpu_ms` did not move. Lavapipe's −19 % (p = 0.07) is also short of
  significance; its quietest blocks gave −14 %.
- Pipelined fps rose on NVIDIA (p < 0.001) and Intel (p = 0.004) and is inconclusive on lavapipe (p = 0.64,
  n = 12). The Vulkan frames did 6-7 % more entities and 8-9.5 % more vertices each, which works against them in
  the fps comparison; on lavapipe that is about the size of the fps change, and Mverts/s is flat (+1.3 %).
  Mverts/s (+26.5 %, +16.6 %, +1.3 %) mixes the two effects and is not the headline.
- No single "X % faster" figure is supported. The defensible statements are: less CPU time per frame on all three
  devices, and higher pipelined throughput on NVIDIA and Intel.
- Spot check after the merge (NVIDIA, sync, n = 8 per side, other processes on 16-23 % of the cores, master's
  SDL_GPU build as A): `cpu_ms` 0.157 → 0.140 (−10.7 %, p = 0.02), `record_ms` −31.8 % (p = 0.02), `frame_ms`
  −3.5 % (p = 0.03); image hash `83a9de542ad33701` on both.

## Risks

- The gain may be small: SDL_GPU already is Vulkan. Measured SDL-driven recording is ~90 of the 150 µs
  CPU frame on the 3080; the other ~100 µs (scene build) is ours and unchanged.
- Sync bugs show up only on some drivers → validation layer on every device, hashes on every device.
- NVIDIA cannot present to the headless surface → the window path there is smoke-tested on Wayland.
- ~4k lines of raw Vulkan to maintain → kept in focused files, no abstraction beyond the four `vk_*` units.
- The D3D backends cannot be run on this machine at all: compile-checked with MinGW-w64 and their
  HLSL compiled with vkd3d-compiler, but correctness, image equality and performance are unverified
  by construction — the user's explicit choice, recorded here so nobody mistakes them for tested.

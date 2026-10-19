# Vulkan backend for `GpuRenderer` — implementation plan

> Execution: inline (executing-plans), tightly coupled single class; subagents only for independent
> slices and for review. Ledger = this file's checkboxes + `git log` on `vulkan-backend`.

**Goal:** direct Vulkan 1.3 replacement of SDL_GPU inside `GpuRenderer`, pixel-identical, faster, A/B-measured.
**Spec:** `docs/superpowers/specs/2026-10-19-vulkan-backend-design.md`
**Worktree:** `/home/jlo/dev/LotusEngine-vulkan` (branch `vulkan-backend`, from `9a85f69`); build dir `build/`.

## Global constraints

- Pixel identity: NVIDIA `a593920ea4013023`, Intel `ce325cdd979df2a7`, lavapipe `b380d9a84f4bde8f`
  (bench frame); `vertices=189132`, `drawn_entities=392`, `draw_calls=72`.
- Vulkan floor 1.3 (dynamicRendering, synchronization2); features: depthClamp, independentBlend.
- Repo style: tabs, `///` docs, no dead code, clean cutover (no SDL_GPU left at the end).
- Tests run synchronously; mutation-check new tests; GPU tests also on lavapipe + Intel.
- Never `git checkout --` uncommitted work; commit after every green task.

## Cheat sheet

```bash
cd /home/jlo/dev/LotusEngine-vulkan
cmake --build build -j24 --target lotus_tests lotus_render_bench lotus && build/bin/lotus_tests
export LVP='VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json LP_NUM_THREADS=8'
export INTEL='VK_DRIVER_FILES=/usr/share/vulkan/icd.d/intel_icd.x86_64.json'
# validation layer (extracted RPM, not system-wide):
export VVL='VK_ADD_LAYER_PATH=/tmp/vvl/root/usr/share/vulkan/explicit_layer.d LD_LIBRARY_PATH=/tmp/vvl/root/usr/lib64 LOTUS_VK_VALIDATION=1'
# A/B: /tmp/lotus_ab/ab_sys.sh N A B ; ab_lvp.sh N A B ; ab_pipe.sh N A B  (taskset 0-7, interleaved)
# image hash: bench prints `ASI image_hash=`; lavapipe compares to bench/golden via --golden
```

## Phase 0 — prep on the SDL_GPU backend (so A and B run the same callers)

- [ ] **P1** `GpuRenderer::wait_idle()`, `device_name()`; `headless.cpp`, bench, 3 tests stop using `gpu.device()`.
      Verify: 340 pass; bench prints device name.
- [ ] **P2** UI hooks (`init_ui_backend/shutdown_ui_backend/ui_backend_new_frame/render_ui/viewport_ui_texture`)
      implemented on the SDL backend; `editor.cpp` uses only them; `capture_viewport_rgba()`; `sky_order_gpu_test`
      uses it. Verify: build, tests.
- [ ] **P3** Offscreen throttle on the SDL backend (fence per submit; `begin_frame_offscreen` waits the oldest when
      > 2 outstanding; `wait_idle` drains). Verify: sync bench unchanged (±noise).
- [ ] **P4** bench `--pipelined --seconds S` (METRIC `fps`, `mverts_per_s`, `mean_drawn_entities`); `ab_pipe.sh`.
      Freeze A: `/tmp/lotus_ab/bench_sdl_v2`, `tests_sdl_v2`. Record SDL baseline (sync + pipelined) on 3 devices.

## Phase 1 — Stage 1: Vulkan for the offscreen/headless path

- [ ] **V1** CMake (Vulkan headers: system or SDL's khronos dir; new `src/gpu/vk/*`), `vk_api`, `vk_context`.
      Test `VkContextTest`: device on each ICD, features present, validation error counter == 0.
- [ ] **V2** `vk_memory`, `vk_barrier`. Tests: alloc/free/coalesce, mapped write→GPU copy→readback round trip,
      barrier tracker emits the expected src/dst for a scripted sequence.
- [ ] **V3** Shaders re-laid-out (sets per spec), SPIR-V regenerated (`GLSLANG=/tmp/shadertools/usr/bin/glslangValidator
      ./shaders/build-shaders.sh`); descriptor/pipeline layouts; pipeline builders for lit/shadow/sky/fxaa/ssao/hiz.
- [ ] **V4** Renderer core (frame ring, begin/end/wait_idle, offscreen colour, depth, dynamic buffers, garbage list);
      geometry (mesh cache, instances, immediate stream, draw recording); shadow + lit + sky.
      Verify: lit frame with fxaa/ssao off equals the SDL build's bytes.
- [ ] **V5** FXAA, SSAO, Hi-Z compute + readback + adopt (cached readback, streaming-load fallback), `capture_rgba`,
      `resize_offscreen`, viewport pass + `capture_viewport_rgba`, all degradation flags/messages.
- [ ] **V6** Callers on the final API; full suite + validation clean on 3 devices; hashes identical.
- [ ] **V7** **A/B checkpoint**: sync + pipelined, 3 devices, report. (Decision point for stage 2.)

## Phase 2 — Stage 2: window, editor, cleanup

- [ ] **W1** Swapchain/present/resize; headless-surface test on lavapipe + Intel; `LOTUS_PRESENT_MODE`.
- [ ] **W2** `imgui_impl_vulkan` (dynamic rendering) behind the UI hooks; `ImGui_ImplSDL3_InitForVulkan`;
      `SDL_WINDOW_VULKAN`; smoke on the Wayland session (launch, N frames, clean exit, validation clean).
- [ ] **W3** Remove SDL_GPU and `imgui_impl_sdlgpu3` from the tree/CMake; README + AGENTS.md + env table.
- [ ] **W4** Whole-branch review (reviewer agent), fix wave, merge into the autoresearch branch, log the experiment.

## Phase 3 — Vulkan-only optimisations (each A/B'd vs the faithful port)

Fused 4×4 Hi-Z dispatch (one dispatch, mip K only); skip the linear-depth clear when the sky covers it;
descriptor-set caching; barrier batching; mesh arena if binds show up; `kFramesInFlight` 2 vs 3.

## Phase D — hand-written D3D backends (compile-checked here, unverified by construction)

Decided 2026-10-19: `d3d12` for Windows 10+, `d3d11` for Windows 7/8 and pre-2015 Intel GPUs,
both behind the same seam; SDL_GPU is not retained. Toolchain on this machine:
`/tmp/mingw/root` (`x86_64-w64-mingw32-g++` 16.2.1; set
`CPLUS_INCLUDE_PATH=/tmp/mingw/root/usr/x86_64-w64-mingw32/sys-root/mingw/include`) and
`/tmp/vkd3d/root/usr/bin/vkd3d-compiler` (needs `LD_LIBRARY_PATH=/tmp/vkd3d/root/usr/lib64`;
target type is `dxbc-tpf`, source type `hlsl`).

- [ ] **D1** Seam: `LOTUS_GPU_BACKEND=vulkan|d3d12|d3d11` selects the implementation files for
      `GpuRenderer`; the public header stays the only interface; Vulkan keeps building.
- [ ] **D2** `scripts/compile-check-d3d.sh`: extracts the MinGW toolchain if missing and compiles
      every `src/gpu/d3d*/` translation unit; kept in the loop while writing D3D code.
- [ ] **D3** HLSL ports (lit.vert/frag, shadow.vert, sky.vert/frag, fxaa.vert/frag, ssao.vert/frag,
      depth.comp) + `shaders/build-shaders-d3d.sh` -> DXBC SM 5.1 headers.
- [ ] **D4** `d3d12`: device/queue/swapchain (DXGI), descriptor heaps, root signature, PSOs,
      upload/default heaps, barrier mapping from the Vulkan design, the same passes, Hi-Z compute,
      readback, ImGui via `imgui_impl_dx12`.
- [ ] **D5** `d3d11`: same feature set with D3D11 state mapping (input layouts, `Map`/`Unmap`
      dynamic buffers, no explicit barriers, FL 10/11 fallbacks), ImGui via `imgui_impl_dx11`.
- [ ] **D6** Docs: README backend matrix with the unverified banner; per-platform build notes.

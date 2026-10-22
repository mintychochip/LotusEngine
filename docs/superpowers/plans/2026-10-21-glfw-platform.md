# Replace SDL3 with GLFW — implementation plan

> Execution: inline (executing-plans). Ledger = this file's checkboxes + `git log` on `glfw-platform`. Every commit
> builds and passes the suite.

**Goal:** the engine builds and runs with no SDL3: GLFW 3.5.1 (window, input, Vulkan surface) and Vulkan-Headers come
from submodules, and the engine opens the Vulkan loader itself.
**Spec:** `docs/superpowers/specs/2026-10-21-glfw-platform-design.md`
**Worktree:** `/home/jlo/dev/LotusEngine-glfw` (branch `glfw-platform`, from `fc00731`); build dir `build/`.

## Global constraints

- Repo style: tabs, `///` docs, Allman function braces, no dead code, clean cutover (no SDL shim, no alias).
- Nothing outside `src/core/application/window.cpp` (+ its GLFW translation unit) and `src/ui/editor.cpp` includes a
  GLFW header. Public headers forward-declare `struct GLFWwindow;` at most.
- Vulkan stays loaded at run time only: nothing links libvulkan.
- Tests that need a Vulkan device keep the skip/`LOTUS_REQUIRE_VULKAN` contract; none needs a display.
- Never `git add -A`; stage explicit paths. Do not push.

## Cheat sheet

```bash
cd /home/jlo/dev/LotusEngine-glfw
# This machine has no X11/Wayland -devel packages and no root: /tmp/devroot is the extracted -devel RPMs
# (libX11, Xrandr, Xinerama, Xcursor, Xi, Xext, Xrender, Xfixes, xorg-x11-proto, wayland, libxkbcommon, libffi).
# On a normal machine `dnf install` / `apt install` of the README's list replaces it.
export PKG_CONFIG_PATH=/tmp/devroot/usr/lib64/pkgconfig:/tmp/devroot/usr/share/pkgconfig
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/devroot/usr \
      -DWAYLAND_SCANNER_EXECUTABLE=/tmp/devroot/usr/bin/wayland-scanner
cmake --build build -j24 --target lotus lotus_rpg lotus_render_bench lotus_tests
export LVP='VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json LP_NUM_THREADS=8'
export VVL='VK_ADD_LAYER_PATH=/tmp/vvl/root/usr/share/vulkan/explicit_layer.d LD_LIBRARY_PATH=/tmp/vvl/root/usr/lib64 LOTUS_VK_VALIDATION=sync'
```

## Tasks

- [x] **T1 Vendor GLFW and Vulkan-Headers.** `.gitmodules`; `dependencies/glfw` at tag 3.5.1,
      `dependencies/vulkan-headers` at v1.3.302; root `CMakeLists.txt` adds GLFW (`GLFW_BUILD_EXAMPLES/TESTS/DOCS/
      INSTALL` off); `cmake/FindLotusVulkanHeaders.cmake` points at the vendored headers only.
      Verify: configure, full build, suite unchanged (SDL is still in use).
- [x] **T2 Engine input types.** `include/lotus/input/key.hpp` + `src/input/key.cpp`: `Key` (physical keys, US-layout
      names), `Mod` (`None, Shift, Ctrl, Alt, Gui` with `|` `&`), `key_name(Key)`, `key_from_name(string_view)`
      (case-insensitive; SDL's names for the keys keybinds already store, `Enter`/`Esc` accepted as aliases).
      `include/lotus/input/input.hpp` + `src/input/input.cpp`: `MouseButton`, `KeyEvent`, `MouseButtonEvent`,
      `MouseMoveEvent`, `Event` (a `std::variant`), `Input` (`apply(Event)`, `key_down`, `mouse_down`, `mouse_x/y`,
      `keys()`); no `release_all()`, since GLFW itself sends the releases when focus is lost.
      Tests `test/input_test.cpp`: names round-trip for every key, parse is case-insensitive, unknown names fail, SDL's
      names kept, `Input` follows press/release/repeat and mouse events, the key snapshot is a copy.
- [ ] **T3 Own Vulkan loader.** `src/gpu/vk/vk_loader.{hpp,cpp}`: `vk::Loader` opens the system loader
      (`libvulkan.so.1`, macOS and Windows names under `#if`) and exposes `vkGetInstanceProcAddr`. `vk::Context` owns
      one instead of calling `SDL_Vulkan_LoadLibrary`; `test/vulkan_device.hpp` probes through it; `headless.cpp`, the
      bench and every GPU test that only needed SDL for the loader drop `SDL_Init`/`SDL_SetHint`. The windowed path
      keeps SDL for window, extensions and surface until T4.
      Verify: suite on three devices, `lotus --headless` screenshots identical to before.
- [ ] **T4 Cutover to GLFW.**
      - `include/lotus/gpu/present_target.hpp`: abstract `PresentTarget` (`pixel_size`, `vulkan_instance_extensions`,
        `create_vulkan_surface(void* instance, void* get_instance_proc) -> uint64_t`).
      - `include/lotus/application/window.hpp` + `src/core/application/window.cpp`: `Window` on GLFW implementing
        `PresentTarget`, owning an `Input`, `poll_events(function<void(const Event&)>)`, `LOTUS_WINDOW_PLATFORM`;
        `src/core/application/glfw_input.{hpp,cpp}`: pure GLFW-key/mods/button translation.
      - `GpuRenderer::init`/`begin_frame` take `PresentTarget*`; `vk::ContextOptions::target`; surface created through
        it, destroyed with `vkDestroySurfaceKHR` (add to the function table); extensions from it.
      - `Editor::init(GLFWwindow*, ...)` on `imgui_impl_glfw` with callbacks installed; `process_event` removed.
      - `Application`: no SDL init/quit; steady_clock timing; `input()`; event callback on engine events; `AudioSystem`
        and `Global` removed.
      - `Keybinds`/`KeyChord` on `Key`/`Mod`/`KeyEvent`; `apply_character_movement_input` takes `const Input&`;
        `RPGGame` reads `input()`.
      - Tests: swapchain test on a `HeadlessPresentTarget` (`VK_EXT_headless_surface`); keybinds and player movement
        tests on engine events; GLFW translation totality test.
      Verify: suite on three devices with validation; windowed run on Wayland and X11.
- [ ] **T5 Delete SDL.** Remove the `dependencies/SDL` submodule, `cmake/FindLotusSDL3.cmake`, SDL3 from
      `CMakeLists.txt`/`imgui` target; CI installs the X11/Wayland -dev packages instead of `libsdl3-dev`; README
      (dependencies, env table), architecture docs, this spec's results.
      Verify: `grep -ri sdl` over the tracked tree finds only history in docs/specs; fresh-clone build.
- [ ] **T6 Verification.** Whole suite on RTX 3080, Intel, lavapipe with sync validation; `lotus` and `lotus_rpg` on
      Wayland and X11 with validation (frames, close via compositor, editor visible); fresh clone configure+build+ctest
      with only the documented packages; both ctest entries including `LOTUS_REQUIRE_VULKAN=1` on lavapipe alone.

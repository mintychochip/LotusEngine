# Replace SDL3 with GLFW — design

Status: requested by the user on 2026-10-21 ("we're moving to Vulkan", so SDL3 goes; "find a way to spawn a window,
keyboard etc"). Branch `glfw-platform` (worktree `../LotusEngine-glfw`), based on `fc00731`.

## Goal

The engine no longer depends on SDL3. The window, the keyboard and mouse, and the Vulkan surface come from **GLFW
3.5.1**, built from a submodule like every other dependency. The Vulkan headers come from a vendored **Vulkan-Headers**
submodule, and the engine opens the Vulkan loader itself. A clean checkout builds with a compiler, CMake and the
platform's X11/Wayland development packages: no SDL, no Vulkan SDK.

Why: SDL3 is not packaged for Ubuntu 24.04, the CI runner, so CI cannot install it. Since SDL_GPU was removed, SDL3
only did platform work, and that is a small, replaceable surface (below). GLFW is the usual Vulkan window library and
compiles from source in under a second.

Acceptance (all must hold):

1. No `SDL` in `src/`, `include/`, `test/`, `bench/`, `CMakeLists.txt`, `cmake/`, `.github/`; the `dependencies/SDL`
   submodule is gone.
2. The whole suite passes on RTX 3080, Intel ARL and lavapipe with the Khronos validation layer clean, as before. The
   GPU tests and `lotus --headless` need no display.
3. `lotus` opens a window and renders, editor included, on Wayland (niri) and on X11; keyboard and mouse reach the
   game and ImGui; closing the window ends the program.
4. A fresh clone builds from scratch with only the documented system packages.

## What SDL3 did, and what replaces it

| SDL3 use | Replacement |
|---|---|
| `SDL_CreateWindow`, `SDL_PollEvent`, quit/close events | `lotus::Window` on GLFW: `glfwCreateWindow`, `glfwPollEvents`; callbacks queue engine events |
| `SDL_GetKeyboardState`, `SDL_Scancode` (player, RPG) | `lotus::Input`, the key state the window keeps from key events, and `lotus::Key` |
| `SDL_Keycode`, `SDL_Keymod`, `SDL_GetKeyName`/`FromName` (keybinds) | `Key`, `Mod`, `key_name()`/`key_from_name()`; the names stay the ones `keybinds.json` already holds |
| `SDL_GetMouseState`, `SDL_EVENT_MOUSE_MOTION` | `Input` mouse position and buttons; `MouseMoveEvent` with deltas |
| `SDL_GetPerformanceCounter`, `SDL_Delay` | `std::chrono::steady_clock`, `std::this_thread::sleep_for` |
| `SDL_Vulkan_LoadLibrary`, `SDL_Vulkan_GetVkGetInstanceProcAddr` | `vk::Loader`: `dlopen`/`LoadLibrary` of the Vulkan loader |
| `SDL_Vulkan_GetInstanceExtensions`, `CreateSurface`, `DestroySurface` | `PresentTarget`: a window answers with `glfwGetRequiredInstanceExtensions` and `glfwCreateWindowSurface`, tests with `VK_EXT_headless_surface`; `vkDestroySurfaceKHR` through the function table |
| `SDL_GetWindowSizeInPixels` | `PresentTarget::pixel_size` (`glfwGetFramebufferSize`) |
| `imgui_impl_sdl3` | `imgui_impl_glfw`, its callbacks chained after the window's |
| `SDL_INIT_AUDIO` in `AudioSystem` | removed: the class only opened a device and played nothing (its header says so) |
| `SDL_HINT_VIDEO_DRIVER=offscreen` in headless, bench and tests | not needed: offscreen rendering never touches a window system |
| Vulkan headers bundled under `dependencies/SDL/src/video/khronos` | `dependencies/vulkan-headers`, tag v1.3.302 |

## Design

- **Engine-owned input types** (`include/lotus/input/`): `Key` (a physical key, named like the US layout), `Mod`
  (shift, ctrl, alt, gui), `MouseButton`, `KeyEvent` and `MouseMoveEvent` in an `Event` variant, and `Input`, the
  pressed-keys and mouse state that applying each event updates. Nothing outside `src/core/application/window.cpp` and
  the editor's ImGui backend sees a GLFW type. Translation from GLFW codes is a pure function with a test that every
  engine key is reachable.
- **`Window`** keeps its shape (`poll_events`, `should_close`, `pixel_size`, `handle`). GLFW callbacks translate to
  engine events, update its `Input`, and queue them; `poll_events` drains the queue into the caller's callback, so the
  per-event ordering the application loop relies on is unchanged. A window initialises GLFW on first use and
  terminates it with the last one, so no caller manages the library. A window that GLFW cannot create (no display)
  throws with GLFW's message. SDL turned SIGINT and SIGTERM into a quit event, and GLFW does not; while a window
  exists it does the same (for signals still at their default), so Ctrl-C and `kill` end the loop and the shutdown
  runs, as before.
- **`PresentTarget`** (`include/lotus/gpu/present_target.hpp`) is what a renderer presents to: pixel size, Vulkan
  instance extensions, and surface creation. `Window` implements it; the swapchain test implements it with a headless
  surface, so the windowed code path is still exercised without a window system. `GpuRenderer::init` and
  `begin_frame` take a `PresentTarget*`. Opaque pointers keep Vulkan headers out of the public header.
- **Application loop**: the event callback is the one it has today, on engine events. ImGui receives events straight
  from the callbacks the GLFW backend installs after the window's (the backend chains the earlier ones), so
  `Editor::process_event` goes away. `captures_keyboard()` and `captures_mouse()` already return false while the
  editor is hidden, so game input is unaffected by ImGui seeing events in that state.
- **Player and RPG input** read the `Input` they are handed (`apply_character_movement_input` takes it; `Application`
  exposes `input()`), which makes the movement code testable for the first time: a test presses `W` and checks the
  velocity.
- **Headless** (`lotus --headless`, the bench, GPU tests) opens the Vulkan loader and never initialises a window system.
- **`LOTUS_WINDOW_PLATFORM=wayland|x11|null`** selects GLFW's platform (it replaces `SDL_VIDEO_DRIVER`). Unset, GLFW
  follows the session (`XDG_SESSION_TYPE`) and otherwise tries Wayland, then X11.

## Behaviour changes, stated plainly

- **Key identity.** Keybinds used SDL keycodes, which follow the active layout; GLFW key tokens follow physical
  position, named after the US layout (WASD movement already worked this way). On a non-US layout "Ctrl+Z" is now the
  key in the US Z position. `keybinds.json` files keep working: the names are unchanged.
- `SDL_VIDEO_DRIVER` no longer applies; use `LOTUS_WINDOW_PLATFORM`.
- Building compiles GLFW's backends, so Linux needs the X11 and Wayland development packages (README lists them for
  Fedora and Ubuntu). Nothing else is needed from the system.
- `AudioSystem` is gone. Audio playback, when it comes, is a new component with its own dependency.

## Non-goals

Windows and macOS runtime verification (the loader and GLFW code paths exist for them but are not exercised here),
gamepads, multiple windows, relative-mouse capture, IME text entry beyond what ImGui's GLFW backend provides, audio.

## Verification

Unit tests for key names, translation totality, `Input` state transitions, keybind matching, player movement from real
key state, and `Window` fed the callbacks GLFW would call (on GLFW's null platform, no display). The whole suite on the
three devices with validation. Windowed runs on Wayland and X11 with validation, close via the compositor, and the
editor drawing. A fresh clone built from nothing. The operating-system-to-GLFW leg of the keyboard is checked on X11
by injecting real key events with XTest (a bound key and an unbound control); Wayland and the mouse cannot be
injected here (no `wtype`/`ydotool`, niri offers no virtual keyboard, and XTest pointer events would move the
session's real cursor), so those are checked by running the application and by the tests above.

## Risks

- GLFW's Wayland backend differs from SDL's in window decorations and in how fractional scaling is reported; the
  run on niri is the check.
- Windows and macOS code paths are untested.
- GLFW is a new submodule to keep current; it is pinned to a release tag.

## Results

Measured on the final tree (2026-10-21). Nothing below was run on Windows or macOS, or on GitHub Actions itself (the
branch is not pushed).

| Check | Result |
|---|---|
| Suite, sync validation, Intel ARL | 413 passed |
| Suite, sync validation, RTX 3080 | 411 passed, 2 skipped (NVIDIA offers no `VK_EXT_headless_surface`, so the headless-swapchain tests skip) |
| Suite, sync validation, lavapipe | 411 passed, 2 skipped (Mesa's headless swapchain crashes on llvmpipe; the tests skip there) |
| Suite with no Vulkan device | 386 passed, 27 skipped; with `LOTUS_REQUIRE_VULKAN=1` those 27 fail, as intended; `ctest` 2/2 |
| `lotus --headless` `main` and `jungle`, 120 ticks, 3 devices | screenshot and state dump byte-identical to the SDL build |
| `lotus_render_bench` image hash | NVIDIA `83a9de542ad33701`, Intel `a3bfbe901faae371`, lavapipe `a467d4ea5cdd618d`: the recorded values |
| `lotus` windowed, 300 frames, sync validation, exit 0 | Wayland (niri) on RTX 3080 and Intel; X11 (Xwayland) on RTX 3080 |
| `lotus` windowed on X11 with Intel | fails at `vkCreateSwapchainKHR` (`VK_ERROR_INITIALIZATION_FAILED`), identically with the SDL build: Mesa on this Xwayland, not this change |
| `lotus_rpg` windowed, 200 frames, sync validation, Wayland | exit 0 |
| Editor | a capture of the Wayland window shows hierarchy, inspector, stats and the rendered viewport |
| Close | closing the window through the compositor exits 0; SIGTERM exits 0 and prints the frame count, as the SDL build did |
| Keyboard on X11 | injected F8 (unbound) reached the engine as `Key::F8` with no action; injected F7 (bound to `quit`) arrived as `Key::F7`, resolved to `quit` and the process exited 0 |
| Fresh clone on Ubuntu 24.04 | the install step of `.github/workflows/ci.yml` verbatim, then configure, build and `LOTUS_REQUIRE_VULKAN=1 xvfb-run -a ctest` as CI runs them: 2/2 passed |
| Fresh clone on Fedora 44 | the README's `dnf install` line verbatim, then configure, build and `ctest` with lavapipe: 2/2 passed |

Not verified: mouse events arriving from the operating system (the callbacks are tested, the OS leg is not), keyboard
on Wayland, and the Windows and macOS paths.

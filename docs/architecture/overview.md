# Architecture overview

Lotus is organized around four boundaries: an application shell, scene data,
a renderer, and runtime services. The boundaries are deliberately narrow so
that scene construction, visibility, serialization, and gameplay systems can be
tested without a window or GPU.

## Design boundaries

- **Application shell** — owns the window, frame loop, input dispatch, editor,
  console, and the active `Scene`. `Application::run` is the windowed path;
  `Application::run_headless` is the test and automation path.
- **Scene data** — an EnTT registry plus hierarchy, persistent GUIDs, component
  codecs, meshes, materials, and scene settings. `Scene` performs CPU-only
  visibility and upload preparation before the renderer records anything.
- **Renderer** — `GpuRenderer` is the backend interface and
  `GpuRendererVulkan` (`src/gpu/vulkan/`) implements it directly on Vulkan 1.3:
  the device, pipelines, textures, buffers, render targets, and pass
  sequencing. It consumes draw batches; it does not know about gameplay or
  serialization.
- **Runtime services** — physics, scripting, animation, input/camera, region
  residency, save games, networking, audio, and tooling. They operate on a
  `Scene` and are driven in a defined order by the application.

## Subsystem map

```mermaid
flowchart TB
    Host["Host application"] --> App["Application<br/>window + frame loop"]
    App --> SceneManager["SceneManager<br/>named scenes + active scene"]
    SceneManager --> Scene["Scene<br/>EnTT registry + hierarchy"]
    Scene --> Assets["AssetRegistry<br/>stable IDs to paths"]
    Scene --> Serialization["Serialization<br/>versioned JSON + codecs"]
    Scene --> Prefabs["Prefabs + additive merge<br/>GUID remap"]
    App --> Renderer["GpuRenderer<br/>Vulkan 1.3"]
    Renderer --> GPU["Vulkan driver"]
    App --> Services["Runtime services"]
    Services --> Physics["PhysicsWorld<br/>Jolt"]
    Services --> Scripts["ScriptEngine<br/>Lua"]
    Services --> Animation["AnimationLibrary"]
    Services --> Input["Keybinds + camera controller"]
    Services --> Residency["ResidencySystem<br/>region streaming"]
    Services --> Tooling["Editor + console<br/>hot reload + async load"]
    Services --> Optional["Optional modules<br/>networking + audio"]
```

## Frame lifecycle

The windowed loop polls events, advances simulation, then prepares and submits
one frame. The editor renders after the scene pass so it can present the scene
in a viewport or in the window.

```mermaid
sequenceDiagram
    autonumber
    participant OS as OS / window
    participant App as Application
    participant Scene as Scene
    participant Phys as PhysicsWorld
    participant Lua as ScriptEngine
    participant Res as ResidencySystem
    participant GPU as GpuRenderer
    participant UI as Editor

    OS->>App: input and window events
    App->>App: on_update(dt)
    App->>App: poll hot reload and async scene load
    App->>Scene: character movement input
    App->>Scene: animation_system_update
    App->>Scene: Scene::update
    App->>Phys: step(scene, dt)
    App->>Lua: update(scene, dt)
    App->>Scene: update_world_transforms
    App->>Scene: update_follow_camera
    App->>Lua: poll(scene)
    App->>Res: update(scene, viewer)
    App->>Scene: upload(gpu, aspect)
    App->>GPU: begin_scene_pass
    App->>Scene: draw(gpu)
    App->>GPU: end_scene_pass
    App->>UI: draw and render
    App->>GPU: end_frame
```

`Scene::update` propagates world transforms and resolves meshes and LOD
geometry. Physics then steps and writes dynamic transforms back; scripts run
after physics. A final world-transform publish runs before the follow camera,
so the renderer sees one consistent transform state for the frame.

## Headless path

`Application::run_headless` uses the same scene and renderer code but skips the
window, editor, audio, and hot reload. It can:

- load a scene,
- run a fixed number of simulation ticks,
- execute console commands,
- dump the final scene JSON,
- render one offscreen frame to a screenshot.

The offscreen renderer follows the same `begin_frame_offscreen` →
`upload` → `begin_scene_pass` → `draw` → `end_scene_pass` → `end_frame`
sequence as the windowed path.

## Testing seams

- `Scene::build_vertices` is pure CPU work: culling, LOD selection, and vertex
  generation can be tested without a GPU.
- `ComponentCodecs` makes serialization extensible without editing a central
  parser.
- `AssetRegistry` separates stable asset IDs from filesystem paths.
- `PhysicsWorld`, `ScriptEngine`, and `ResidencySystem` operate on a `Scene`
  and can be exercised independently.
- GPU tests cover instanced versus immediate geometry and batch grouping.
- GPU tests need a Vulkan 1.3 device (lavapipe is enough) and skip, saying why,
  when there is none; `LOTUS_REQUIRE_VULKAN=1` turns the skip into a failure.

## Related documents

- [Rendering pipeline](rendering.md)
- [Scenes, assets, and streaming](scenes-and-assets.md)
- [Runtime services](runtime-services.md)

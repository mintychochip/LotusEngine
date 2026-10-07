# Skeletal animation (phase 2)

## Goal

Import skinned meshes and clip playback from glTF/GLB, building on transform
keyframes and the asset registry.

## Non-goals (this phase)

- IK, retargeting, animation events, blend trees beyond two-clip lerp.

## Data model

- `Skeleton` component: joint names, parent indices, inverse bind matrices.
- `SkinnedMesh` component: joint entity GUIDs or joint indices, mesh + weights.
- `SkinAnimator`: `clip_id`, time, speed, loop (parallel to `Animator`).

## Pipeline

1. GLB: read `skins`, `nodes`, `animations` (sampler/channel).
2. Bake clips to JSON (same track shape as transform clips) or binary sidecar.
3. CPU skinning into `GpuVertex` stream (or GPU skinning later).
4. `skin_animation_system_update` before physics (moving platforms rule).

## Dependencies

- Stable joint GUIDs in scene documents (`SceneGuid` per joint entity or
  skeleton root + named joints).
- Material batching unchanged; skinned draws may be per-entity until instancing.

## Verification

- Unit: clip sampling on one joint.
- Integration: GLB with single bone, one translation keyframe, mesh follows.

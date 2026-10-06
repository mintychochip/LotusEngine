# Scene Prefabs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add prefabs and instantiation to the scene subsystem, on top of the stable entity identity they require, plus two foundation defects that block both.

**Architecture:** A prefab is a scene fragment stored under `prefabs/<name>.json` in the same versioned format as a scene, so the existing codec round-trips it unchanged — no second format. Instantiating clones the fragment into the target scene, assigns fresh GUIDs, and rewrites internal `EntityRef`s to the new GUIDs. Every load path stays atomic by building into a temporary and swapping, which is what `scene_from_json` already does.

**Tech Stack:** C++23, EnTT (submodule) behind `include/lotus/ecs.hpp`, nlohmann/json, GoogleTest. CMake with `GLOB_RECURSE ... CONFIGURE_DEPENDS`, so no CMake edits.

**Spec:** `docs/superpowers/specs/2026-10-05-scene-prefabs-design.md`

## Global Constraints

These apply to every task. Copy them verbatim.

- Baseline is **148 passing tests** (`build/bin/lotus_tests`). Never leave it lower.
- Preserve existing public signatures, or migrate every caller in the same change. No dead code, no shims, no compatibility aliases.
- Every load and instantiate path is atomic: build into a temporary, then swap. A malformed prefab, missing asset, or unresolved reference leaves the live scene untouched and is reported via `last_mesh_error()`.
- Unknown components are skipped, never fatal. A prefab containing one still instantiates.
- Derived fields (`Transform::world`, `Hierarchy`, `Name`) are never serialized.
- `Entity` stays `entt::entity`; do not replace the ECS or introduce raw indices.
- The format evolves forward only: keep a migration hook, reject only versions newer than the current one.
- Determinism: `names()`, serialized key order, and any enumeration are stable.
- Do NOT change occlusion culling, LOD, or frustum culling. No per-frame cost added to `Scene::build_vertices` beyond load-time reference resolution.
- Do NOT start any workstream listed under "Deferred" in the spec.
- Build: `cd build && cmake --build . -j"$(nproc)"`. Test binary: `build/bin/lotus_tests`.

## File Structure

| File | Responsibility |
|---|---|
| `src/scene/scene_manager.cpp` | Sort `names()`; deterministic scene enumeration. |
| `include/lotus/scene/serialization.hpp` / `src/scene/serialization.cpp` | Format version 2, the migration hook, GUID and reference codecs. |
| `include/lotus/scene/components.hpp` | `SceneGuid`, `EntityRef`. |
| `src/scene/scene.cpp` | GUID assignment, `resolve_refs()`. |
| `include/lotus/scene/prefab.hpp` / `src/scene/prefab.cpp` | `Prefab` load and `instantiate`. |
| `src/ui/editor.cpp` | Create-prefab and instantiate entries. |
| `test/scene_test.cpp`, `test/scene_serialization_test.cpp`, `test/scene_prefab_test.cpp` | Cases for each task. |

---

### Task 0: Foundation defects

Two small, independent fixes. Both block later tasks: identity needs a serialized key that survives a reload, and a format change needs a migration path.

**Files:**
- Modify: `src/scene/scene_manager.cpp`
- Modify: `src/scene/serialization.cpp`, `include/lotus/scene/serialization.hpp`
- Test: `test/scene_test.cpp`, `test/scene_serialization_test.cpp`

**Interfaces:**
- Produces: `SceneManager::names()` returns sorted order; `bool lotus::migrate_scene_json(nlohmann::json&, int from, int to)`; `kSceneFormatVersion == 2`.

- [ ] **Step 1: Write the failing tests**

In `test/scene_test.cpp`, add to `SceneManagerTest`:

```cpp
TEST(SceneManagerTest, NamesAreSortedAndStable)
{
	SceneManager manager;
	manager.create("zulu");
	manager.create("alpha");
	manager.create("mike");

	const std::vector<std::string> names = manager.names();
	ASSERT_EQ(names.size(), 3u);
	EXPECT_EQ(names[0], "alpha");
	EXPECT_EQ(names[1], "mike");
	EXPECT_EQ(names[2], "zulu");
}
```

In `test/scene_serialization_test.cpp`, add:

```cpp
TEST(SceneSerialization, Version1DocumentMigratesToCurrent)
{
	// A version-1 document must load under the current format through the
	// migration hook, not be rejected.
	const std::string text = R"({
	  "version": 1,
	  "name": "old",
	  "entities": [
	    {"name": "cube", "components": {"Transform": {"position": [1.0, 2.0, 3.0]}}}
	  ]
	})";

	Scene scene{"s"};
	ASSERT_TRUE(scene_from_json(scene, text, nullptr));
	EXPECT_EQ(scene.name(), "old");
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);
}

TEST(SceneSerialization, FutureVersionIsRejected)
{
	const std::string text = R"({"version": 99, "name": "future", "entities": []})";
	Scene scene{"s"};
	std::string error;
	EXPECT_FALSE(scene_from_json(scene, text, &error));
	EXPECT_FALSE(error.empty());
}
```

- [ ] **Step 2: Run the tests to confirm they fail**

Run: `cd build && cmake --build . -j"$(nproc)" && ./bin/lotus_tests --gtest_filter='SceneManagerTest.NamesAreSortedAndStable:SceneSerialization.Version1DocumentMigratesToCurrent'`
Expected: `NamesAreSortedAndStable` fails (unordered order); the migration test fails if version 1 is currently rejected for a changed format — otherwise it passes and only guards the path.

- [ ] **Step 3: Sort `names()`**

In `src/scene/scene_manager.cpp`, add `#include <algorithm>` and sort before returning:

```cpp
std::vector<std::string> SceneManager::names() const
{
	std::vector<std::string> out;
	out.reserve(scenes_.size());
	for (const auto& [name, scene] : scenes_) {
		(void)scene;
		out.push_back(name);
	}
	// The header promises sorted order; unordered_map iteration is arbitrary.
	std::sort(out.begin(), out.end());
	return out;
}
```

- [ ] **Step 4: Add the migration hook and bump the version**

In `include/lotus/scene/serialization.hpp`, bump the version and declare the hook:

```cpp
/// Version of the on-disk scene format. Bump when the schema changes.
inline constexpr int kSceneFormatVersion = 2;

/// Upgrades a parsed document from \p from to \p to in place. Returns false
/// when no path exists. Called by scene_from_json before any component is read,
/// so a document written by an older version loads unchanged.
bool migrate_scene_json(nlohmann::json& root, int from, int to);
```

In `src/scene/serialization.cpp`, define it and change the guard:

```cpp
bool migrate_scene_json(nlohmann::json& root, int from, int to)
{
	if (from == to) {
		return true;
	}
	if (from < 1 || from > to) {
		return false; // no downgrade path, and nothing below version 1
	}
	// 1 -> 2: no schema change. The hook exists so the next bump has a home.
	for (int version = from; version < to; ++version) {
		switch (version) {
		case 1:
			break; // 1 -> 2 is a no-op
		default:
			return false;
		}
	}
	root["version"] = to;
	return true;
}
```

Change the version check in `scene_from_json`:

```cpp
	const int version = root.value("version", 0);
	if (version <= 0) {
		return fail("scene document has no version");
	}
	if (version > kSceneFormatVersion) {
		return fail("unsupported scene version: " + std::to_string(version));
	}
	if (!migrate_scene_json(root, version, kSceneFormatVersion)) {
		return fail("cannot migrate scene from version " +
		            std::to_string(version));
	}
```

- [ ] **Step 5: Build and run the full suite**

Run: `cd build && cmake --build . -j"$(nproc)" && ./bin/lotus_tests`
Expected: all tests pass, count ≥ 148.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "Sort SceneManager::names and add a scene-format migration hook"
```

---

### Task 1: Stable identity and references

**Files:**
- Modify: `include/lotus/scene/components.hpp`, `src/scene/scene.cpp`, `src/scene/serialization.cpp`
- Test: `test/scene_serialization_test.cpp`

**Interfaces:**
- Consumes: the migration hook and version 2 from Task 0.
- Produces: `SceneGuid { uint64_t value; }`, `EntityRef { SceneGuid target; }`, `Scene::guid_of(Entity)`, `Scene::resolve_refs()`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(SceneSerialization, GuidRoundTripsThroughSaveLoad)
{
	Scene scene{"s"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.registry().emplace<EntityRef>(a, EntityRef{scene.guid_of(b)});

	const std::string text = to_json_string(scene);
	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));

	const Entity la = loaded.find("a");
	const Entity lb = loaded.find("b");
	ASSERT_NE(la, NullEntity);
	ASSERT_NE(lb, NullEntity);
	ASSERT_TRUE(loaded.registry().all_of<EntityRef>(la));
	EXPECT_EQ(loaded.registry().get<EntityRef>(la).target.value,
	          loaded.guid_of(lb).value);
}

TEST(SceneSerialization, UnresolvedReferenceIsReportedNotFatal)
{
	Scene scene{"s"};
	const Entity a = scene.create("a");
	scene.registry().emplace<EntityRef>(a, EntityRef{SceneGuid{0xDEADBEEF}});

	const std::string text = to_json_string(scene);
	Scene loaded{"s"};
	EXPECT_TRUE(scene_from_json(loaded, text, nullptr));
	EXPECT_FALSE(loaded.last_mesh_error().empty());
	EXPECT_NE(loaded.find("a"), NullEntity); // scene still valid
}
```

- [ ] **Step 2: Run the tests to confirm they fail**

Run: `./bin/lotus_tests --gtest_filter='SceneSerialization.GuidRoundTrips*:SceneSerialization.UnresolvedReference*'`
Expected: compile error (types do not exist yet).

- [ ] **Step 3: Add the types**

In `include/lotus/scene/components.hpp`:

```cpp
/// A persistent identity that survives save/load and reordering. Zero means
/// unassigned; Scene::create assigns one and load regenerates a missing one.
struct SceneGuid {
	uint64_t value{0};
};

/// Names another entity by GUID rather than by handle, so a reference survives
/// a reload. Resolved to a handle by Scene::resolve_refs().
struct EntityRef {
	SceneGuid target;
};
```

- [ ] **Step 4: Assign and resolve GUIDs**

In `src/scene/scene.cpp`, add a GUID map and the accessors. `create()` assigns `SceneGuid{++next_guid_}` and stores it; `resolve_refs()` walks `EntityRef` components and fills `last_mesh_error_` for any target with no match. Call `resolve_refs()` at the end of a successful `scene_from_json`.

- [ ] **Step 5: Serialize both components**

Register `SceneGuid` (hex string) and `EntityRef` (hex string) in `default_component_codecs()`. Derived/regenerated fields stay out where applicable.

- [ ] **Step 6: Build, run the full suite, verify in the app**

Run: `cd build && cmake --build . -j"$(nproc)" && ./bin/lotus_tests`
Then: `./bin/lotus`, confirm `scenes/main.json` round-trips with GUIDs present and no unresolved-reference warnings in the F2 console.

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "Add SceneGuid and EntityRef with atomic reference resolution"
```

---

### Task 2: Prefabs and instancing

**Files:**
- Create: `include/lotus/scene/prefab.hpp`, `src/scene/prefab.cpp`
- Modify: `src/ui/editor.cpp`
- Test: `test/scene_prefab_test.cpp`

**Interfaces:**
- Consumes: `SceneGuid`, `EntityRef`, `resolve_refs()` from Task 1; the codec and atomic loader.
- Produces: `Prefab`, `load_prefab(path)`, `Entity instantiate(const Prefab&, Scene&, Entity parent = NullEntity)`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(PrefabTest, TwoInstancesAreIndependent)
{
	Scene scene{"s"};
	const Prefab prefab = prefab_from_json(R"({
	  "version": 2, "name": "rock",
	  "entities": [{"name": "rock", "components": {
	    "Transform": {"position": [0.0, 0.0, 0.0]}, "Material": {"base_color": [1.0, 0.0, 0.0]}}}]
	})");

	const Entity a = instantiate(prefab, scene);
	const Entity b = instantiate(prefab, scene);
	ASSERT_NE(a, b);
	ASSERT_NE(scene.guid_of(a).value, scene.guid_of(b).value);

	scene.registry().get<Transform>(a).position.x = 5.0f;
	EXPECT_EQ(scene.registry().get<Transform>(b).position.x, 0.0f);
}

TEST(PrefabTest, InternalReferencesAreRewrittenPerInstance)
{
	// A prefab whose child refs its parent must point at the right parent in
	// each instance, not at the first instance's entities.
	// (Build the fragment, instantiate twice, assert each EntityRef resolves
	//  within its own instance.)
}

TEST(PrefabTest, MissingAssetReportsWithoutCorruption)
{
	// A prefab referencing a missing mesh instantiates; the scene stays valid
	// and last_mesh_error() is non-empty.
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `./bin/lotus_tests --gtest_filter='PrefabTest.*'`
Expected: compile error (no prefab API).

- [ ] **Step 3: Implement `Prefab` and `instantiate`**

`Prefab` holds the parsed fragment. `instantiate` clones entities and components into the target scene through the codec path, assigns fresh GUIDs, remaps internal `EntityRef`s, and calls `resolve_refs()`. Atomic: build the clone fully, then attach.

- [ ] **Step 4: Editor entries**

Add "Create prefab from selection" and an instantiate entry in the Hierarchy panel, matching the existing button style.

- [ ] **Step 5: Build, run the full suite, verify in the app**

Run: `cd build && cmake --build . -j"$(nproc)" && ./bin/lotus_tests`
Then: `./bin/lotus`, create a prefab, instantiate it twice, move one instance, confirm the other is unaffected and the F2 console is clean.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "Add prefabs and instantiation over the scene codec"
```

---

**Type consistency:** `SceneGuid` is identical across `components.hpp`, the codecs, and `Prefab`. `instantiate(prefab, scene, parent)` matches across Tasks 2's declaration, definition, and the editor call site. `migrate_scene_json(json&, int, int)` matches between Task 0's declaration and `scene_from_json`.

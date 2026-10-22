#include "lotus/ui/editor.hpp"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstring>
#include <utility>

#include "lotus/console/console.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/input/keybinds.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/assets.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/scene_manager.hpp"

namespace lotus {
namespace {

std::string label_for(Scene& scene, Entity entity)
{
	const auto& name = scene.registry().get<Name>(entity).value;
	char buffer[64];
	if (name.empty()) {
		std::snprintf(buffer,
		              sizeof(buffer),
		              "Entity %u",
		              static_cast<unsigned>(entt::to_integral(entity)));
	} else {
		std::snprintf(buffer,
		              sizeof(buffer),
		              "%s##%u",
		              name.c_str(),
		              static_cast<unsigned>(entt::to_integral(entity)));
	}
	return buffer;
}

/// Viewer selection helpers live here when needed.

} // namespace

Editor::~Editor()
{
	shutdown();
}

bool Editor::init(GLFWwindow* window, GpuRenderer& gpu)
{
	if (initialized_) {
		return true;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

	// The platform backend only needs to know which graphics API the window
	// serves; the renderer backend itself is owned by the GpuRenderer behind
	// its UI hooks. The platform backend installs its GLFW callbacks after the
	// window's own and chains to them, so the window still sees every event.
	if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
		return false;
	}
	if (!gpu.init_ui_backend()) {
		ImGui_ImplGlfw_Shutdown();
		return false;
	}
	initialized_ = true;
	gpu_         = &gpu;
	return true;
}

void Editor::shutdown()
{
	if (!initialized_) {
		return;
	}
	gpu_->shutdown_ui_backend();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	initialized_ = false;
}

bool Editor::captures_keyboard() const
{
	if (!initialized_ || !visible_) {
		return false;
	}
	return ImGui::GetIO().WantCaptureKeyboard;
}

bool Editor::captures_mouse() const
{
	if (!initialized_ || !visible_) {
		return false;
	}
	return ImGui::GetIO().WantCaptureMouse;
}


void Editor::begin_frame()
{
	if (!initialized_) {
		return;
	}
	ImGui_ImplGlfw_NewFrame();
	gpu_->ui_backend_new_frame();
	ImGui::NewFrame();
	frame_open_ = true;
}

void Editor::draw(SceneManager& scenes, float dt)
{
	if (!initialized_ || !visible_) {
		return;
	}
	Scene* scene = scenes.active();
	if (scene) {
		if (selected_ != NullEntity && !scene->registry().valid(selected_)) {
			selected_ = NullEntity;
		}
	}

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::SetNextWindowBgAlpha(1.0f);
	ImGui::Begin("EditorRoot",
	             nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                     ImGuiWindowFlags_NoResize |
	                     ImGuiWindowFlags_NoBringToFrontOnFocus);

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	// Panels take a share of the window rather than a fixed width, so a narrow
	// window still leaves the viewport a usable centre column. The side panels
	// are additionally capped at half the width between them.
	float hierarchy_width = std::clamp(avail.x * 0.22f, 150.0f, 300.0f);
	float inspector_width = std::clamp(avail.x * 0.26f, 170.0f, 340.0f);
	const float sides     = hierarchy_width + inspector_width;
	if (sides > avail.x * 0.5f) {
		const float scale = (avail.x * 0.5f) / sides;
		hierarchy_width *= scale;
		inspector_width *= scale;
	}
	const float bottom_height = std::clamp(avail.y * 0.24f, 130.0f, 260.0f);
	const float top_height    = avail.y - bottom_height;

	// Hierarchy (left)
	ImGui::BeginChild("HierarchyChild", ImVec2(hierarchy_width, top_height), true);
	ImGui::TextUnformatted("Hierarchy");
	ImGui::Separator();
	if (scene) {
		draw_hierarchy(*scene);
	} else {
		ImGui::TextDisabled("No active scene");
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// Viewport (center)
	const float viewport_width = avail.x - hierarchy_width - inspector_width;
	ImGui::BeginChild("ViewportChild", ImVec2(viewport_width, top_height), true);
	if (scene != nullptr) {
		draw_viewport(*scene);
	} else {
		ImGui::TextDisabled("No active scene");
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// Inspector (right)
	ImGui::BeginChild("InspectorChild", ImVec2(inspector_width, top_height), true);
	ImGui::TextUnformatted("Inspector");
	ImGui::Separator();
	if (scene) {
		draw_inspector(*scene);
	}
	ImGui::EndChild();

	// Bottom row
	ImGui::BeginChild("BottomChild", ImVec2(0, bottom_height), true);
	const float bottom_avail = ImGui::GetContentRegionAvail().x;
	const float col_width    = bottom_avail / 5.0f;

	ImGui::BeginChild("StatsChild", ImVec2(col_width, 0), true);
	ImGui::TextUnformatted("Stats");
	ImGui::Separator();
	draw_stats(scenes, dt);
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("SceneChild", ImVec2(col_width, 0), true);
	ImGui::TextUnformatted("Scene File");
	ImGui::Separator();
	draw_scene_panel();
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("AssetsChild", ImVec2(col_width, 0), true);
	ImGui::TextUnformatted("Assets");
	ImGui::Separator();
	draw_assets(scene);
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("KeybindsChild", ImVec2(col_width, 0), true);
	ImGui::TextUnformatted("Keybinds");
	ImGui::Separator();
	draw_keybinds();
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("ConsoleChild", ImVec2(0, 0), true);
	ImGui::TextUnformatted("Console");
	ImGui::Separator();
	draw_console();
	ImGui::EndChild();

	ImGui::EndChild(); // BottomChild

	ImGui::End(); // EditorRoot
}

void Editor::draw_viewport(Scene& scene)
{
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	if (avail.x <= 1.0f || avail.y <= 1.0f) {
		return;
	}
	// Recorded before the readiness check: the application sizes the offscreen
	// target from this, so a frame that cannot draw yet still reports the size
	// the next frame must allocate.
	viewport_size_.width  = static_cast<uint32_t>(avail.x);
	viewport_size_.height = static_cast<uint32_t>(avail.y);

	if (gpu_ == nullptr || !gpu_->viewport_ready()) {
		ImGui::TextDisabled("Viewport not ready");
		return;
	}

	ImGui::Image(ImTextureID(gpu_->viewport_ui_texture()), avail);
	const ImVec2 image_min  = ImGui::GetItemRectMin();
	const ImVec2 image_max  = ImGui::GetItemRectMax();
	const ImVec2 image_size{image_max.x - image_min.x, image_max.y - image_min.y};
	if (image_size.x <= 1.0f || image_size.y <= 1.0f) {
		return;
	}
	const float aspect = image_size.x / image_size.y;

	ImGuiIO& io         = ImGui::GetIO();
	const ImVec2 mouse  = io.MousePos;
	const bool hovered  = ImGui::IsItemHovered();
	const ImVec2 local{mouse.x - image_min.x, mouse.y - image_min.y};
	const float ndc_x = 2.0f * (local.x / image_size.x) - 1.0f;
	const float ndc_y = 1.0f - 2.0f * (local.y / image_size.y);

	// Camera orbit and zoom. The scene camera is view state, not scene data,
	// so orbiting never marks the scene edited.
	if (Camera3D* camera = scene.active_camera3d()) {
		if (hovered &&
		    (ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
		     ImGui::IsMouseDown(ImGuiMouseButton_Middle))) {
			glm::vec3 offset = camera->position - camera->target;
			const float radius = glm::length(offset);
			if (radius > 0.0001f) {
				float theta = std::atan2(offset.x, offset.z);
				float phi   = std::acos(glm::clamp(offset.y / radius, -1.0f, 1.0f));
				theta -= io.MouseDelta.x * 0.01f;
				phi = glm::clamp(phi - io.MouseDelta.y * 0.01f, 0.05f, 3.09f);
				offset = glm::vec3{radius * std::sin(phi) * std::sin(theta),
				                   radius * std::cos(phi),
				                   radius * std::sin(phi) * std::cos(theta)};
				camera->position = camera->target + offset;
			}
		}
		if (hovered && io.MouseWheel != 0.0f) {
			const glm::vec3 offset = camera->position - camera->target;
			camera->position = camera->target + offset * std::pow(0.9f, io.MouseWheel);
		}
	}

	// Click to select: a left-click picks the entity under the cursor, or
	// clears the selection when it hit nothing. Picking is read-only.
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		if (physics_ != nullptr) {
			selected_ = scene.pick(ndc_x, ndc_y, aspect, physics_,
			                       Scene::PickMode::PhysicsOnly);
			if (selected_ == NullEntity) {
				selected_ = scene.pick(ndc_x, ndc_y, aspect, nullptr,
				                       Scene::PickMode::BoundsOnly);
			}
		} else {
			selected_ = scene.pick(ndc_x, ndc_y, aspect, nullptr,
			                       Scene::PickMode::BoundsOnly);
		}
	}
}

void Editor::draw_console()
{
	if (!console_ || !console_visible_) {
		return;
	}
	const float footer = ImGui::GetStyle().ItemSpacing.y +
	                     ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("##log", ImVec2(0, -footer), ImGuiChildFlags_Borders);
	for (const ConsoleEntry& entry : console_->entries()) {
		ImVec4 color{0.85f, 0.85f, 0.85f, 1.0f};
		if (entry.level == LogLevel::Warning) {
			color = ImVec4{1.0f, 0.8f, 0.3f, 1.0f};
		} else if (entry.level == LogLevel::Error) {
			color = ImVec4{1.0f, 0.4f, 0.4f, 1.0f};
		}
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(entry.text.c_str());
		ImGui::PopStyleColor();
	}
	if (console_scroll_) {
		ImGui::SetScrollHereY(1.0f);
		console_scroll_ = false;
	}
	ImGui::EndChild();

	if (refocus_console_) {
		ImGui::SetKeyboardFocusHere();
		refocus_console_ = false;
	}
	ImGui::SetNextItemWidth(-1);
	if (ImGui::InputTextWithHint("##command",
	                             "command (try: help)",
	                             console_input_,
	                             sizeof(console_input_),
	                             ImGuiInputTextFlags_EnterReturnsTrue)) {
		const std::string line = console_input_;
		if (!line.empty()) {
			console_history_.push_back(line);
			console_->execute(line);
			console_scroll_ = true;
		}
		console_input_[0]    = '\0';
		console_history_pos_ = -1;
		refocus_console_     = true;
	}
	if (ImGui::IsItemActive() && !console_history_.empty()) {
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
			if (console_history_pos_ < 0) {
				console_history_pos_ = static_cast<int>(
				        console_history_.size());
			}
			if (console_history_pos_ > 0) {
				--console_history_pos_;
				std::snprintf(console_input_,
				              sizeof(console_input_),
				              "%s",
				              console_history_[console_history_pos_]
				                      .c_str());
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
			if (console_history_pos_ >= 0 &&
			    console_history_pos_ + 1 <
			            static_cast<int>(console_history_.size())) {
				++console_history_pos_;
				std::snprintf(console_input_,
				              sizeof(console_input_),
				              "%s",
				              console_history_[console_history_pos_]
				                      .c_str());
			} else {
				console_history_pos_ = -1;
				console_input_[0]    = '\0';
			}
		}
	}
}

void Editor::draw_hierarchy(Scene& scene)
{
	// Read-only: the tree below selects entities; use the console
	// (e.g. scene.save) for every mutation.
	ImGui::Separator();
	for (const Entity root : scene.roots()) {
		draw_entity_node(scene, root);
	}
}

void Editor::draw_entity_node(Scene& scene, Entity entity)
{
	const auto& hierarchy = scene.registry().get<Hierarchy>(entity);
	const bool is_leaf    = (hierarchy.first_child == NullEntity);

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
	                           ImGuiTreeNodeFlags_SpanAvailWidth |
	                           ImGuiTreeNodeFlags_DefaultOpen;
	if (is_leaf) {
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}
	if (entity == selected_) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const std::string label = label_for(scene, entity);
	const bool open         = ImGui::TreeNodeEx(label.c_str(), flags);
	if (ImGui::IsItemClicked()) {
		selected_ = entity;
	}
	if (open && !is_leaf) {
		for (Entity child = hierarchy.first_child; child != NullEntity;
		     child = scene.registry().get<Hierarchy>(child).next_sibling) {
			draw_entity_node(scene, child);
		}
		ImGui::TreePop();
	}
}

void Editor::draw_inspector(Scene& scene)
{
	if (selected_ == NullEntity || !scene.registry().valid(selected_)) {
		ImGui::TextUnformatted("Nothing selected");
		return;
	}
	// Read-only: every value below is display text. Nothing here writes
	// the registry; mutate scenes through console commands instead.
	const auto& registry = scene.registry();

	ImGui::Text("Name: %s", registry.get<Name>(selected_).value.c_str());

	if (const auto* transform = registry.try_get<Transform>(selected_)) {
		ImGui::SeparatorText("Transform (structural)");
		ImGui::Text("Position: %.3f, %.3f, %.3f", transform->position.x,
		            transform->position.y, transform->position.z);
		ImGui::Text("Rotation: %.3f, %.3f, %.3f", transform->rotation.x,
		            transform->rotation.y, transform->rotation.z);
		ImGui::Text("Scale: %.3f, %.3f, %.3f", transform->scale.x,
		            transform->scale.y, transform->scale.z);
	}
	if (const auto* sprite = registry.try_get<Sprite>(selected_)) {
		ImGui::SeparatorText("Sprite");
		ImGui::Text("Color: %.3f, %.3f, %.3f", sprite->color.x,
		            sprite->color.y, sprite->color.z);
		ImGui::Text("Size: %.3f x %.3f", sprite->size.x, sprite->size.y);
	}
	if (const auto* material = registry.try_get<Material>(selected_)) {
		ImGui::SeparatorText("Material");
		ImGui::Text("Base color: %.3f, %.3f, %.3f", material->base_color.x,
		            material->base_color.y, material->base_color.z);
		ImGui::Text("Albedo id: %s", material->albedo_id.c_str());
		ImGui::Text("Roughness: %.3f", material->roughness);
	}
	if (const auto* controller =
	            registry.try_get<CharacterController>(selected_)) {
		ImGui::SeparatorText("CharacterController");
		ImGui::Text("Radius: %.3f", controller->radius);
		ImGui::Text("Half height: %.3f", controller->half_height);
		ImGui::Text("Desired velocity: %.3f, %.3f, %.3f",
		            controller->desired_velocity.x,
		            controller->desired_velocity.y,
		            controller->desired_velocity.z);
		ImGui::Text("Grounded: %s", controller->grounded ? "yes" : "no");
	}
	if (const auto* body = registry.try_get<RigidBody>(selected_)) {
		ImGui::SeparatorText("RigidBody");
		ImGui::Text("Motion: %d", static_cast<int>(body->motion_type));
		ImGui::Text("Mass: %.3f", body->mass);
		ImGui::Text("Friction: %.3f", body->friction);
		ImGui::Text("Restitution: %.3f", body->restitution);
	}
	if (const auto* box = registry.try_get<BoxCollider>(selected_)) {
		ImGui::SeparatorText("BoxCollider");
		ImGui::Text("Half extents: %.3f, %.3f, %.3f", box->half_extents.x,
		            box->half_extents.y, box->half_extents.z);
	}
	if (const auto* sphere = registry.try_get<SphereCollider>(selected_)) {
		ImGui::SeparatorText("SphereCollider");
		ImGui::Text("Radius: %.3f", sphere->radius);
	}
	if (const auto* mesh_source = registry.try_get<MeshSource>(selected_)) {
		ImGui::SeparatorText("MeshSource");
		ImGui::Text("Path: %s", mesh_source->path.c_str());
		ImGui::Text("Asset id: %s", mesh_source->id.c_str());
	}
	if (const auto* animator = registry.try_get<Animator>(selected_)) {
		ImGui::SeparatorText("Animator");
		ImGui::Text("Clip id: %s", animator->clip_id.c_str());
		ImGui::Text("Speed: %.3f", animator->speed);
		ImGui::Text("Loop: %s", animator->loop ? "yes" : "no");
		ImGui::Text("Playing: %s", animator->playing ? "yes" : "no");
	}
	if (const auto* camera3d = registry.try_get<Camera3D>(selected_)) {
		ImGui::SeparatorText("Camera3D");
		ImGui::Text("Position: %.3f, %.3f, %.3f", camera3d->position.x,
		            camera3d->position.y, camera3d->position.z);
		ImGui::Text("Target: %.3f, %.3f, %.3f", camera3d->target.x,
		            camera3d->target.y, camera3d->target.z);
		ImGui::Text("FOV: %.1f", camera3d->fov_degrees);
		ImGui::Text("Near: %.3f  Far: %.1f", camera3d->near_plane,
		            camera3d->far_plane);
	}
	if (const auto* camera = registry.try_get<Camera2D>(selected_)) {
		ImGui::SeparatorText("Camera2D");
		ImGui::Text("Position: %.3f, %.3f", camera->position.x,
		            camera->position.y);
		ImGui::Text("Zoom: %.3f", camera->zoom);
	}
}

void Editor::draw_stats(SceneManager& scenes, float dt)
{
	const Scene* scene = scenes.active();
	ImGui::Text("Scene: %s", scene ? scene->name().c_str() : "(none)");
	ImGui::Text("Scenes: %zu", scenes.count());
	ImGui::Text("Entities: %zu",
	            scene ? scene->registry().view<Transform>().size() : 0u);
	ImGui::Text("Vertices: %u", scene ? scene->vertex_count() : 0u);
	ImGui::Text("Draw calls: %u", scene ? scene->draw_calls() : 0u);
	ImGui::Text("Drawn: %u  Culled: %u  Occluded: %u",
	            scene ? scene->drawn_entities() : 0u,
	            scene ? scene->culled_entities() : 0u,
	            scene ? scene->occluded_entities() : 0u);
	ImGui::Text("Frame: %.2f ms (%.0f FPS)", dt * 1000.0f, ImGui::GetIO().Framerate);
}

void Editor::draw_scene_panel()
{
	if (!scene_controls_.path) {
		return;
	}
	ImGui::TextWrapped("File: %s", scene_controls_.path().c_str());
	if (scene_controls_.status) {
		ImGui::TextWrapped("%s", scene_controls_.status().c_str());
	}
}

void Editor::draw_assets(Scene* scene)
{
	if (!asset_controls_.registry) {
		return;
	}
	if (asset_controls_.status) {
		ImGui::TextWrapped("%s", asset_controls_.status().c_str());
	}
	if (asset_controls_.dangling_refs && scene != nullptr) {
		const std::vector<std::string> dangling =
		        asset_controls_.dangling_refs(scene);
		for (const std::string& id : dangling) {
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
			                   "dangling: %s", id.c_str());
		}
	}
	const AssetRegistry* registry = asset_controls_.registry();
	if (registry == nullptr) {
		return;
	}
	ImGui::BeginChild("AssetList", ImVec2(0, 0), false);
	for (const std::string& id : registry->names()) {
		const std::string* path = registry->find(id);
		ImGui::TextUnformatted(id.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("->");
		ImGui::SameLine();
		ImGui::TextWrapped("%s", path != nullptr ? path->c_str() : "");
	}
	ImGui::EndChild();
}

void Editor::draw_keybinds()
{
	if (keybinds_ == nullptr) {
		ImGui::TextDisabled("No keybinds");
		return;
	}
	ImGui::BeginChild("KeybindList", ImVec2(0, 0), false);
	for (const auto& [action, chord] : keybinds_->bindings()) {
		ImGui::TextUnformatted(action.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("->");
		ImGui::SameLine();
		ImGui::TextUnformatted(chord.to_string().c_str());
	}
	ImGui::EndChild();
}

void Editor::render(GpuRenderer& gpu)
{
	if (!initialized_ || !frame_open_) {
		return;
	}
	frame_open_ = false;
	ImGui::Render();
	gpu.render_ui(ImGui::GetDrawData());
}

} // namespace lotus

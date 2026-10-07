#include "lotus/ui/editor.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
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

/// Gizmo handle length and pick radius, both in screen pixels.
constexpr float kGizmoPixels     = 90.0f;
constexpr float kGizmoPickPixels = 9.0f;

/// Shortest distance from \p p to the segment \p a - \p b, in screen pixels.
float distance_to_segment(const ImVec2& p, const ImVec2& a, const ImVec2& b)
{
	const ImVec2 ab{b.x - a.x, b.y - a.y};
	const ImVec2 ap{p.x - a.x, p.y - a.y};
	const float length_sq = ab.x * ab.x + ab.y * ab.y;
	if (length_sq <= 0.0001f) {
		return std::sqrt(ap.x * ap.x + ap.y * ap.y);
	}
	float t = (ap.x * ab.x + ap.y * ab.y) / length_sq;
	t       = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	const ImVec2 closest{a.x + ab.x * t, a.y + ab.y * t};
	const ImVec2 delta{p.x - closest.x, p.y - closest.y};
	return std::sqrt(delta.x * delta.x + delta.y * delta.y);
}

} // namespace

Editor::~Editor()
{
	shutdown();
}

bool Editor::init(SDL_Window* window, GpuRenderer& gpu)
{
	if (initialized_) {
		return true;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

	if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
		return false;
	}
	ImGui_ImplSDLGPU3_InitInfo info{};
	info.Device            = gpu.device();
	info.ColorTargetFormat = gpu.swapchain_format();
	info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
	if (!ImGui_ImplSDLGPU3_Init(&info)) {
		ImGui_ImplSDL3_Shutdown();
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
	ImGui_ImplSDLGPU3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	initialized_ = false;
}

void Editor::set_scene_controls(SceneControls controls)
{
	scene_controls_ = std::move(controls);
}

void Editor::process_event(const SDL_Event& event)
{
	if (!initialized_) {
		return;
	}
	ImGui_ImplSDL3_ProcessEvent(&event);
}

bool Editor::captures_keyboard() const
{
	if (!initialized_ || !visible_) {
		return false;
	}
	return ImGui::GetIO().WantCaptureKeyboard;
}

void Editor::set_keybinds(Keybinds* keybinds, std::function<void()> on_save)
{
	keybinds_      = keybinds;
	save_keybinds_ = std::move(on_save);
}

void Editor::begin_frame()
{
	if (!initialized_ || !visible_) {
		return;
	}
	ImGui_ImplSDL3_NewFrame();
	ImGui_ImplSDLGPU3_NewFrame();
	ImGui::NewFrame();
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
		if (scene != history_scene_) {
			history_.reset(*scene);
			history_scene_ = scene;
		}
		if (undo_requested_) {
			if (history_.undo(*scene)) {
				dirty_ = true;
			}
			undo_requested_ = false;
		}
		if (redo_requested_) {
			if (history_.redo(*scene)) {
				dirty_ = true;
			}
			redo_requested_ = false;
		}
		if (edited_) {
			history_.commit(*scene);
			dirty_  = true;
			edited_ = false;
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

	ImGui::BeginChild("PrefabsChild", ImVec2(col_width, 0), true);
	ImGui::TextUnformatted("Prefabs");
	ImGui::Separator();
	if (scene) {
		draw_prefabs(*scene);
	}
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

	ImGui::Image(ImTextureID(gpu_->viewport_texture()), avail);
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

	// Camera navigation. The scene camera is a component, not a transform, so
	// this writes its fields directly and never touches the undo history.
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

	// The same matrices pick() builds, so the handles land exactly where the
	// ray for a click at that pixel would go.
	const glm::mat4 view_projection = [&] {
		if (const Camera3D* camera = scene.active_camera3d()) {
			return camera->view_projection(aspect);
		}
		static const Camera2D fallback{};
		const Camera2D* camera2d = scene.active_camera();
		const Camera2D& chosen   = camera2d ? *camera2d : fallback;
		return chosen.view_projection(aspect);
	}();

	const auto project = [&](const glm::vec3& world, ImVec2& out) {
		const glm::vec4 clip = view_projection * glm::vec4{world, 1.0f};
		if (clip.w <= 0.0001f) {
			return false;
		}
		const glm::vec3 ndc = glm::vec3{clip} / clip.w;
		out.x = image_min.x + (ndc.x * 0.5f + 0.5f) * image_size.x;
		out.y = image_min.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * image_size.y;
		return true;
	};

	const bool has_selection =
	        selected_ != NullEntity && scene.registry().valid(selected_);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	ImVec2 origin_screen{0.0f, 0.0f};
	ImVec2 handle_screen[3]{};
	glm::vec3 axis_world[3]{};
	float pixels_per_unit[3]{0.0f, 0.0f, 0.0f};
	bool handle_visible[3]{false, false, false};
	bool origin_visible = false;

	if (has_selection) {
		const Transform& transform = scene.registry().get<Transform>(selected_);
		const glm::vec3 origin{transform.world[3]};
		origin_visible = project(origin, origin_screen);
		if (origin_visible) {
			for (int axis = 0; axis < 3; ++axis) {
				const glm::vec3 column{transform.world[axis]};
				if (glm::length(column) <= 0.0001f) {
					continue;
				}
				axis_world[axis] = glm::normalize(column);
				ImVec2 unit_screen{0.0f, 0.0f};
				if (!project(origin + axis_world[axis], unit_screen)) {
					continue;
				}
				const float dx = unit_screen.x - origin_screen.x;
				const float dy = unit_screen.y - origin_screen.y;
				pixels_per_unit[axis] = std::sqrt(dx * dx + dy * dy);
				if (pixels_per_unit[axis] <= 0.0001f) {
					continue;
				}
				// Fixed pixel length, so the gizmo does not grow as the camera
				// pulls back or as an entity is scaled.
				const float handle_world = kGizmoPixels / pixels_per_unit[axis];
				handle_visible[axis] =
				        project(origin + axis_world[axis] * handle_world,
				                handle_screen[axis]);
			}
		}
	}

	const auto axis_hovered = [&](int axis) {
		return handle_visible[axis] && gizmo_axis_ == axis;
	};

	// A click on empty space clears the selection, which is what every
	// hierarchy-plus-viewport editor does. Local: it never spans a frame.
	bool viewport_clicked = false;

	// Begin a drag on a handle, or select what is under the cursor.
	if (has_selection && origin_visible && gizmo_axis_ < 0 && hovered &&
	    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		int hit       = -1;
		float nearest = kGizmoPickPixels;
		for (int axis = 0; axis < 3; ++axis) {
			if (!handle_visible[axis]) {
				continue;
			}
			const float distance = distance_to_segment(mouse, origin_screen,
			                                           handle_screen[axis]);
			if (distance < nearest) {
				nearest = distance;
				hit     = axis;
			}
		}
		if (hit >= 0) {
			gizmo_axis_         = hit;
			gizmo_dragging_     = true;
			gizmo_last_mouse_x_ = mouse.x;
			gizmo_last_mouse_y_ = mouse.y;
		} else {
			viewport_clicked = true;
		}
	} else if (hovered && gizmo_axis_ < 0 &&
	           ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		viewport_clicked = true;
	}

	// A click that did not land on a handle selects the entity under it, or
	// clears the selection when it hit nothing.
	if (viewport_clicked && !gizmo_dragging_) {
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

	// Drag the handle. The movement is applied per frame along the axis, then
	// world transforms are refreshed so the next frame's handle follows.
	if (gizmo_dragging_ && gizmo_axis_ >= 0 && has_selection &&
	    handle_visible[gizmo_axis_]) {
		const float dx = mouse.x - gizmo_last_mouse_x_;
		const float dy = mouse.y - gizmo_last_mouse_y_;
		gizmo_last_mouse_x_ = mouse.x;
		gizmo_last_mouse_y_ = mouse.y;

		const ImVec2 axis_screen{handle_screen[gizmo_axis_].x - origin_screen.x,
		                         handle_screen[gizmo_axis_].y - origin_screen.y};
		const float axis_length =
		        std::sqrt(axis_screen.x * axis_screen.x + axis_screen.y * axis_screen.y);
		if (axis_length > 0.0001f) {
			const ImVec2 unit{axis_screen.x / axis_length, axis_screen.y / axis_length};
			const float pixels  = dx * unit.x + dy * unit.y;
			const float world   = pixels / pixels_per_unit[gizmo_axis_];
			const glm::vec3 world_delta = axis_world[gizmo_axis_] * world;

			glm::mat4 parent_world{1.0f};
			if (const Hierarchy* hierarchy = scene.registry().try_get<Hierarchy>(selected_);
			    hierarchy != nullptr && hierarchy->parent != NullEntity) {
				parent_world = scene.registry().get<Transform>(hierarchy->parent).world;
			}
			Transform& transform = scene.registry().get<Transform>(selected_);
			transform.position +=
			        glm::vec3{glm::inverse(parent_world) * glm::vec4{world_delta, 0.0f}};
			update_world_transforms(scene);
			dirty_ = true;
		}
	}
	if (gizmo_dragging_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		gizmo_dragging_ = false;
		gizmo_axis_     = -1;
		// One history step for the whole drag, taken now that it has settled.
		edited_ = true;
	}

	// Draw the handles last so they sit over the rendered image.
	if (has_selection && origin_visible) {
		static const ImU32 kAxisColor[3] = {
		        IM_COL32(226, 76, 76, 255),
		        IM_COL32(96, 200, 96, 255),
		        IM_COL32(80, 132, 235, 255)};
		const ImU32 highlight = IM_COL32(255, 214, 64, 255);
		for (int axis = 0; axis < 3; ++axis) {
			if (!handle_visible[axis]) {
				continue;
			}
			const ImU32 color = axis_hovered(axis) ? highlight : kAxisColor[axis];
			draw_list->AddLine(origin_screen, handle_screen[axis], color, 3.0f);
			draw_list->AddCircleFilled(handle_screen[axis], 5.0f, color);
		}
		draw_list->AddCircleFilled(origin_screen, 3.0f, IM_COL32(240, 240, 240, 255));
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

void Editor::draw_keybinds()
{
	if (!keybinds_) {
		return;
	}
	if (ImGui::Button("Save keybinds") && save_keybinds_) {
		save_keybinds_();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset to defaults")) {
		keybinds_->reset_to_defaults();
	}
	ImGui::Separator();
	for (const auto& [action, chord] : keybinds_->bindings()) {
		ImGui::Text("%s", action.c_str());
		ImGui::SameLine(170.0f);
		ImGui::TextDisabled("%s", chord.to_string().c_str());
	}
}

void Editor::draw_hierarchy(Scene& scene)
{
	if (ImGui::Button("Add root")) {
		selected_ = scene.create("entity");
		edited_   = true;
	}
	const bool has_selection = selected_ != NullEntity &&
	                           scene.registry().valid(selected_);
	ImGui::SameLine();
	if (ImGui::Button("Add child") && has_selection) {
		const Entity child = scene.create("entity");
		scene.set_parent(child, selected_);
		selected_ = child;
		edited_   = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Destroy") && has_selection) {
		scene.destroy(selected_);
		selected_ = NullEntity;
		edited_   = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Unparent") && has_selection) {
		scene.detach(selected_);
		edited_ = true;
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!history_.can_undo());
	if (ImGui::Button("Undo")) {
		undo_requested_ = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!history_.can_redo());
	if (ImGui::Button("Redo")) {
		redo_requested_ = true;
	}
	ImGui::EndDisabled();
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
	auto& registry = scene.registry();

	auto& name = registry.get<Name>(selected_).value;
	char buffer[128];
	std::snprintf(buffer, sizeof(buffer), "%s", name.c_str());
	if (ImGui::InputText("Name", buffer, sizeof(buffer))) {
		name = buffer;
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		edited_ = true;
	}

	if (auto* transform = registry.try_get<Transform>(selected_)) {
		ImGui::SeparatorText("Transform (structural)");
		ImGui::DragFloat3("Position", &transform->position.x, 0.01f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat3("Rotation", &transform->rotation.x, 0.01f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat3("Scale", &transform->scale.x, 0.01f, 0.001f, 100.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* sprite = registry.try_get<Sprite>(selected_)) {
		ImGui::SeparatorText("Sprite");
		ImGui::ColorEdit3("Color", &sprite->color.x);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat2("Size", &sprite->size.x, 0.01f, 0.001f, 100.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* material = registry.try_get<Material>(selected_)) {
		ImGui::SeparatorText("Material");
		ImGui::ColorEdit3("Base color", &material->base_color.x);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		char albedo[128];
		std::snprintf(albedo, sizeof(albedo), "%s", material->albedo_id.c_str());
		if (ImGui::InputText("Albedo id", albedo, sizeof(albedo))) {
			material->albedo_id = albedo;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat("Roughness", &material->roughness, 0.01f, 0.0f, 1.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* controller = registry.try_get<CharacterController>(selected_)) {
		ImGui::SeparatorText("CharacterController");
		ImGui::DragFloat("Radius", &controller->radius, 0.01f, 0.1f, 2.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat("Half height", &controller->half_height, 0.01f, 0.1f,
		                 3.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat3("Desired velocity", &controller->desired_velocity.x,
		                  0.05f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::Text("Grounded: %s", controller->grounded ? "yes" : "no");
	}
	if (auto* body = registry.try_get<RigidBody>(selected_)) {
		ImGui::SeparatorText("RigidBody");
		int motion = static_cast<int>(body->motion_type);
		const char* motion_labels[] = {"Static", "Kinematic", "Dynamic"};
		if (ImGui::Combo("Motion", &motion, motion_labels, 3)) {
			body->motion_type = static_cast<PhysicsMotionType>(motion);
			edited_ = true;
		}
		ImGui::DragFloat("Mass", &body->mass, 0.1f, 0.001f, 1000.0f);
		ImGui::DragFloat("Friction", &body->friction, 0.01f, 0.0f, 2.0f);
		ImGui::DragFloat("Restitution", &body->restitution, 0.01f, 0.0f, 1.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* box = registry.try_get<BoxCollider>(selected_)) {
		ImGui::SeparatorText("BoxCollider");
		ImGui::DragFloat3("Half extents", &box->half_extents.x, 0.01f, 0.01f,
		                  50.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* sphere = registry.try_get<SphereCollider>(selected_)) {
		ImGui::SeparatorText("SphereCollider");
		ImGui::DragFloat("Radius", &sphere->radius, 0.01f, 0.01f, 50.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* mesh_source = registry.try_get<MeshSource>(selected_)) {
		ImGui::SeparatorText("MeshSource");
		char path[256];
		std::snprintf(path, sizeof(path), "%s", mesh_source->path.c_str());
		if (ImGui::InputText("Path", path, sizeof(path))) {
			mesh_source->path = path;
		}
		char mesh_id[128];
		std::snprintf(mesh_id, sizeof(mesh_id), "%s", mesh_source->id.c_str());
		if (ImGui::InputText("Asset id", mesh_id, sizeof(mesh_id))) {
			mesh_source->id = mesh_id;
		}
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* animator = registry.try_get<Animator>(selected_)) {
		ImGui::SeparatorText("Animator");
		char clip[128];
		std::snprintf(clip, sizeof(clip), "%s", animator->clip_id.c_str());
		if (ImGui::InputText("Clip id", clip, sizeof(clip))) {
			animator->clip_id = clip;
		}
		ImGui::DragFloat("Speed", &animator->speed, 0.01f, 0.0f, 10.0f);
		ImGui::Checkbox("Loop", &animator->loop);
		ImGui::Checkbox("Playing", &animator->playing);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* camera3d = registry.try_get<Camera3D>(selected_)) {
		ImGui::SeparatorText("Camera3D");
		ImGui::DragFloat3("Position", &camera3d->position.x, 0.05f);
		ImGui::DragFloat3("Target", &camera3d->target.x, 0.05f);
		ImGui::DragFloat("FOV", &camera3d->fov_degrees, 0.5f, 10.0f, 120.0f);
		ImGui::DragFloat("Near", &camera3d->near_plane, 0.01f, 0.01f, 10.0f);
		ImGui::DragFloat("Far", &camera3d->far_plane, 1.0f, 1.0f, 500.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}
	if (auto* camera = registry.try_get<Camera2D>(selected_)) {
		ImGui::SeparatorText("Camera2D");
		ImGui::DragFloat2("Cam position", &camera->position.x, 0.01f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
		ImGui::DragFloat("Zoom", &camera->zoom, 0.01f, 0.01f, 100.0f);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			edited_ = true;
		}
	}

	ImGui::SeparatorText("Components");
	static const char* kAddable[] = {"Sprite",
	                                 "Camera2D",
	                                 "Camera3D",
	                                 "MeshSource",
	                                 "Material",
	                                 "CharacterController",
	                                 "RigidBody + Box",
	                                 "Animator"};
	static int choice             = 0;
	ImGui::SetNextItemWidth(160);
	ImGui::Combo("##add", &choice, kAddable, 8);
	ImGui::SameLine();
	if (ImGui::Button("Add component")) {
		switch (choice) {
		case 0:
			if (!registry.all_of<Sprite>(selected_)) {
				registry.emplace<Sprite>(selected_);
			}
			break;
		case 1:
			if (!registry.all_of<Camera2D>(selected_)) {
				registry.emplace<Camera2D>(selected_);
			}
			break;
		case 2:
			if (!registry.all_of<Camera3D>(selected_)) {
				registry.emplace<Camera3D>(selected_);
			}
			break;
		case 3:
			if (!registry.all_of<MeshSource>(selected_)) {
				registry.emplace<MeshSource>(
				        selected_, MeshSource{"builtin:cube"});
			}
			break;
		case 4:
			if (!registry.all_of<Material>(selected_)) {
				registry.emplace<Material>(selected_);
			}
			break;
		case 5:
			if (!registry.all_of<CharacterController>(selected_)) {
				registry.emplace<CharacterController>(selected_);
			}
			break;
		case 6:
			if (!registry.all_of<RigidBody>(selected_)) {
				registry.emplace<RigidBody>(selected_);
			}
			if (!registry.all_of<BoxCollider>(selected_)) {
				registry.emplace<BoxCollider>(selected_);
			}
			break;
		case 7:
			if (!registry.all_of<Animator>(selected_)) {
				registry.emplace<Animator>(selected_);
			}
			break;
		default:
			break;
		}
		edited_ = true;
	}
	if (registry.all_of<Sprite>(selected_)) {
		if (ImGui::Button("Remove Sprite")) {
			registry.remove<Sprite>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<Camera2D>(selected_)) {
		if (ImGui::Button("Remove Camera2D")) {
			registry.remove<Camera2D>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<Camera3D>(selected_)) {
		if (ImGui::Button("Remove Camera3D")) {
			registry.remove<Camera3D>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<MeshSource>(selected_)) {
		if (ImGui::Button("Remove MeshSource")) {
			registry.remove<MeshSource>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<Material>(selected_)) {
		if (ImGui::Button("Remove Material")) {
			registry.remove<Material>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<CharacterController>(selected_)) {
		if (ImGui::Button("Remove CharacterController")) {
			registry.remove<CharacterController>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<RigidBody>(selected_)) {
		if (ImGui::Button("Remove RigidBody")) {
			registry.remove<RigidBody>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<BoxCollider>(selected_)) {
		if (ImGui::Button("Remove BoxCollider")) {
			registry.remove<BoxCollider>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<SphereCollider>(selected_)) {
		if (ImGui::Button("Remove SphereCollider")) {
			registry.remove<SphereCollider>(selected_);
			edited_ = true;
		}
	}
	if (registry.all_of<Animator>(selected_)) {
		if (ImGui::Button("Remove Animator")) {
			registry.remove<Animator>(selected_);
			edited_ = true;
		}
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
	if (!scene_controls_.path || !scene_controls_.on_save) {
		return;
	}
	ImGui::TextWrapped("File: %s", scene_controls_.path().c_str());
	if (ImGui::Button("Save")) {
		scene_controls_.on_save();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload")) {
		scene_controls_.on_reload();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Auto-save", &auto_save_);
	if (dirty_) {
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "unsaved changes");
	} else {
		ImGui::TextDisabled("up to date");
	}
	if (scene_controls_.status) {
		ImGui::TextWrapped("%s", scene_controls_.status().c_str());
	}
	if (auto_save_ && dirty_) {
		scene_controls_.on_save();
	}
}

void Editor::draw_assets(Scene* scene)
{
	if (!asset_controls_.registry || !asset_controls_.on_reload) {
		return;
	}
	if (ImGui::Button("Reload")) {
		asset_controls_.on_reload();
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

void Editor::draw_prefabs(Scene& scene)
{
	if (!prefab_controls_.names || !prefab_controls_.instantiate) {
		return;
	}
	const bool has_selection = selected_ != NullEntity &&
	                           scene.registry().valid(selected_);

	ImGui::SetNextItemWidth(160);
	ImGui::InputText("##prefabname", prefab_name_, sizeof(prefab_name_));
	ImGui::SameLine();
	ImGui::BeginDisabled(!has_selection || !prefab_controls_.create_from);
	if (ImGui::Button("Create from selection")) {
		std::string error;
		if (!prefab_controls_.create_from(selected_, prefab_name_, &error)) {
			prefab_error_ = error;
		} else {
			prefab_error_.clear();
		}
	}
	ImGui::EndDisabled();

	ImGui::Separator();
	for (const std::string& name : prefab_controls_.names()) {
		ImGui::TextUnformatted(name.c_str());
		ImGui::SameLine();
		ImGui::PushID(name.c_str());
		if (ImGui::Button("Instantiate")) {
			if (prefab_controls_.instantiate(name, selected_) ==
			    NullEntity) {
				prefab_error_ = "could not instantiate " + name;
			} else {
				prefab_error_.clear();
			}
		}
		ImGui::PopID();
	}
	if (!prefab_error_.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
		                   "%s",
		                   prefab_error_.c_str());
	}
}

void Editor::render(GpuRenderer& gpu)
{
	if (!initialized_ || !visible_) {
		return;
	}
	ImGui::Render();
	ImDrawData* draw_data = ImGui::GetDrawData();
	if (!draw_data || !gpu.command_buffer() || !gpu.swapchain_texture()) {
		return;
	}
	ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, gpu.command_buffer());

	SDL_GPUColorTargetInfo target{};
	target.texture  = gpu.swapchain_texture();
	target.load_op  = SDL_GPU_LOADOP_LOAD;
	target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(gpu.command_buffer(),
	                                                 &target,
	                                                 1,
	                                                 nullptr);
	ImGui_ImplSDLGPU3_RenderDrawData(draw_data, gpu.command_buffer(), pass);
	SDL_EndGPURenderPass(pass);
}

} // namespace lotus

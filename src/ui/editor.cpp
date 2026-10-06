#include "lotus/ui/editor.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "lotus/console/console.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/input/keybinds.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/scene_manager.hpp"

namespace lotus {
namespace {

std::string label_for(Scene& scene, Entity entity)
{
	const auto& name = scene.registry().get<Name>(entity).value;
	char buffer[64];
	if (name.empty()) {
		std::snprintf(buffer, sizeof(buffer), "Entity %u",
		              static_cast<unsigned>(entt::to_integral(entity)));
	} else {
		std::snprintf(buffer, sizeof(buffer), "%s##%u", name.c_str(),
		              static_cast<unsigned>(entt::to_integral(entity)));
	}
	return buffer;
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
	// Key handling is driven by Keybinds in Application; ImGui just sees the
	// event so text fields and shortcuts it owns still work.
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
	keybinds_       = keybinds;
	save_keybinds_  = std::move(on_save);
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
		// A different scene (or a hot reload) invalidates the history.
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
		draw_hierarchy(*scene);
		draw_inspector(*scene);
		if (edited_) {
			history_.commit(*scene);
			dirty_  = true;
			edited_ = false;
		}
	}
	draw_stats(scenes, dt);
	draw_scene_panel();
	draw_console();
	draw_keybinds();
}

void Editor::draw_console()
{
	if (!console_ || !console_visible_) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(680, 300), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Console", &console_visible_)) {
		const float footer =
		        ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
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
		if (ImGui::InputTextWithHint("##command", "command (try: help)",
		                             console_input_, sizeof(console_input_),
		                             ImGuiInputTextFlags_EnterReturnsTrue)) {
			const std::string line = console_input_;
			if (!line.empty()) {
				console_history_.push_back(line);
				console_->execute(line);
				console_scroll_ = true;
			}
			console_input_[0]     = '\0';
			console_history_pos_  = -1;
			refocus_console_      = true;
		}
		if (ImGui::IsItemActive() && !console_history_.empty()) {
			if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
				if (console_history_pos_ < 0) {
					console_history_pos_ =
					        static_cast<int>(console_history_.size());
				}
				if (console_history_pos_ > 0) {
					--console_history_pos_;
					std::snprintf(console_input_,
					              sizeof(console_input_),
					              "%s",
					              console_history_[console_history_pos_].c_str());
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
					              console_history_[console_history_pos_].c_str());
				} else {
					console_history_pos_ = -1;
					console_input_[0]    = '\0';
				}
			}
		}
	}
	ImGui::End();
}

void Editor::draw_keybinds()
{
	if (!keybinds_) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(360, 260), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Keybinds")) {
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
	ImGui::End();
}

void Editor::draw_hierarchy(Scene& scene)
{
	ImGui::SetNextWindowSize(ImVec2(300, 340), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Hierarchy")) {
		if (ImGui::Button("Add root")) {
			selected_ = scene.create("entity");
			edited_   = true;
		}
		const bool has_selection =
		        selected_ != NullEntity && scene.registry().valid(selected_);
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
	ImGui::End();
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
	ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Inspector")) {
		if (selected_ == NullEntity || !scene.registry().valid(selected_)) {
			ImGui::TextUnformatted("Nothing selected");
			ImGui::End();
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

		// Add / remove optional components.
		ImGui::SeparatorText("Components");
		static const char* kAddable[] = {"Sprite", "Camera2D"};
		static int choice             = 0;
		ImGui::SetNextItemWidth(140);
		ImGui::Combo("##add", &choice, kAddable, 2);
		ImGui::SameLine();
		if (ImGui::Button("Add component")) {
			if (choice == 0 && !registry.all_of<Sprite>(selected_)) {
				registry.emplace<Sprite>(selected_);
			} else if (choice == 1 && !registry.all_of<Camera2D>(selected_)) {
				registry.emplace<Camera2D>(selected_);
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
	}
	ImGui::End();
}

void Editor::draw_stats(SceneManager& scenes, float dt)
{
	ImGui::SetNextWindowSize(ImVec2(280, 150), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Stats")) {
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
		ImGui::Text("Frame: %.2f ms (%.0f FPS)", dt * 1000.0f,
		            ImGui::GetIO().Framerate);
	}
	ImGui::End();
}

void Editor::draw_scene_panel()
{
	if (!scene_controls_.path || !scene_controls_.on_save) {
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(440, 150), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Scene File")) {
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
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
			                   "unsaved changes");
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
	ImGui::End();
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
	// Must happen outside a render pass.
	ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, gpu.command_buffer());

	SDL_GPUColorTargetInfo target{};
	target.texture  = gpu.swapchain_texture();
	target.load_op  = SDL_GPU_LOADOP_LOAD;
	target.store_op = SDL_GPU_STOREOP_STORE;

	SDL_GPURenderPass* pass =
	        SDL_BeginGPURenderPass(gpu.command_buffer(), &target, 1, nullptr);
	ImGui_ImplSDLGPU3_RenderDrawData(draw_data, gpu.command_buffer(), pass);
	SDL_EndGPURenderPass(pass);
}

} // namespace lotus

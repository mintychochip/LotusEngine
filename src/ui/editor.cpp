#include "lotus/ui/editor.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <cstdio>
#include <cstring>

#include "lotus/gpu/renderer.hpp"
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

void Editor::process_event(const SDL_Event& event)
{
	if (!initialized_) {
		return;
	}
	if (event.type == SDL_EVENT_KEY_DOWN &&
	    event.key.key == SDLK_F1 && !event.key.repeat) {
		toggle();
	}
	ImGui_ImplSDL3_ProcessEvent(&event);
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
		draw_hierarchy(*scene);
		draw_inspector(*scene);
	}
	draw_stats(scenes, dt);
}

void Editor::draw_hierarchy(Scene& scene)
{
	ImGui::SetNextWindowSize(ImVec2(280, 320), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Hierarchy")) {
		if (ImGui::Button("Add entity")) {
			scene.create("entity");
		}
		ImGui::SameLine();
		if (selected_ != NullEntity && scene.registry().valid(selected_) &&
		    ImGui::Button("Destroy selected")) {
			scene.destroy(selected_);
			selected_ = NullEntity;
		}
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
		     child         = scene.registry().get<Hierarchy>(child).next_sibling) {
			draw_entity_node(scene, child);
		}
		ImGui::TreePop();
	}
}

void Editor::draw_inspector(Scene& scene)
{
	ImGui::SetNextWindowSize(ImVec2(320, 360), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Inspector")) {
		if (selected_ == NullEntity || !scene.registry().valid(selected_)) {
			ImGui::TextUnformatted("Nothing selected");
		} else {
			auto& registry = scene.registry();

			auto& name = registry.get<Name>(selected_).value;
			char buffer[128];
			std::snprintf(buffer, sizeof(buffer), "%s", name.c_str());
			if (ImGui::InputText("Name", buffer, sizeof(buffer))) {
				name = buffer;
			}

			if (auto* transform = registry.try_get<Transform>(selected_)) {
				ImGui::SeparatorText("Transform");
				ImGui::DragFloat3("Position", &transform->position.x, 0.01f);
				ImGui::DragFloat3("Rotation", &transform->rotation.x, 0.01f);
				ImGui::DragFloat3("Scale", &transform->scale.x, 0.01f, 0.001f,
				                  100.0f);
			}
			if (auto* sprite = registry.try_get<Sprite>(selected_)) {
				ImGui::SeparatorText("Sprite");
				ImGui::ColorEdit3("Color", &sprite->color.x);
				ImGui::DragFloat2("Size", &sprite->size.x, 0.01f, 0.001f, 100.0f);
			}
		}
	}
	ImGui::End();
}

void Editor::draw_stats(SceneManager& scenes, float dt)
{
	ImGui::SetNextWindowSize(ImVec2(280, 140), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Stats")) {
		const Scene* scene = scenes.active();
		ImGui::Text("Scene: %s", scene ? scene->name().c_str() : "(none)");
		ImGui::Text("Scenes: %zu", scenes.count());
		ImGui::Text("Entities: %zu",
		            scene ? scene->registry().view<Transform>().size() : 0u);
		ImGui::Text("Vertices: %u", scene ? scene->vertex_count() : 0u);
		ImGui::Text("Draw calls: %u", scene ? scene->draw_calls() : 0u);
		ImGui::Text("Frame: %.2f ms (%.0f FPS)", dt * 1000.0f,
		            ImGui::GetIO().Framerate);
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

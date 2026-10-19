#pragma once

namespace lotus {

/// Which target receives this frame's single scene draw.
enum class SceneDrawTarget {
	/// The window. Used in play mode, and before the editor has a viewport size.
	Window,
	/// The editor's offscreen panel. The window is cleared and not filled with
	/// a second copy of the scene.
	Viewport,
};

/// The editor covers the window, so a live viewport is the only image the user
/// sees. A hidden editor keeps a stale viewport size; that must not keep the
/// offscreen pass running.
inline SceneDrawTarget select_scene_draw_target(bool editor_visible,
                                                int viewport_width,
                                                int viewport_height)
{
	if (editor_visible && viewport_width > 0 && viewport_height > 0) {
		return SceneDrawTarget::Viewport;
	}
	return SceneDrawTarget::Window;
}

} // namespace lotus

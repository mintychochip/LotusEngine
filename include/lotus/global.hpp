#pragma once
#include <bitset>
#include <glad/glad.h>
#include <glfw/glfw3.h>

namespace lotus {
static constexpr size_t GLOBAL_FLAGS{32};
enum GlobalFlagType {
	LOTUS_GLFW_INITIALIZED,
};
class Global {
public:
	static Global& get()
	{
		static Global instance;
		return instance;
	}

	static void flag(size_t flag_type, bool val)
	{
		get().flags_.set(flag_type, val);
	}

	static bool flag(size_t flag_type)
	{
		return get().flags_.test(flag_type);
	}

private:
	Global() {}
	std::bitset<GLOBAL_FLAGS> flags_;
};
} // namespace lotus
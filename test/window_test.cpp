// Window tests on GLFW's null platform, which needs no display: they feed a
// window the callbacks GLFW would call and check what the engine sees. What
// they cannot reach is the operating system handing GLFW a real key press;
// that leg is checked by running the application.

#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

#include <csignal>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lotus/application/window.hpp"

namespace lotus {

/// Calls the window's GLFW callbacks the way GLFW would.
struct WindowTestAccess {
	static void key(Window& window, int key, int action, int mods = 0)
	{
		Window::on_key(window.handle(), key, 0, action, mods);
	}
	static void button(Window& window, int button, int action)
	{
		Window::on_mouse_button(window.handle(), button, action, 0);
	}
	static void cursor(Window& window, double x, double y)
	{
		Window::on_cursor(window.handle(), x, y);
	}
	static void enter(Window& window, bool entered)
	{
		Window::on_cursor_enter(window.handle(), entered ? 1 : 0);
	}
	static void close(Window& window) { Window::on_close(window.handle()); }
};

} // namespace lotus

using namespace lotus;

namespace {

/// Sets LOTUS_WINDOW_PLATFORM for a test and puts the old value back.
class PlatformEnv {
public:
	explicit PlatformEnv(const char* value)
	{
		if (const char* old = std::getenv("LOTUS_WINDOW_PLATFORM")) {
			had_old_ = true;
			old_     = old;
		}
		setenv("LOTUS_WINDOW_PLATFORM", value, 1);
	}
	~PlatformEnv()
	{
		if (had_old_) {
			setenv("LOTUS_WINDOW_PLATFORM", old_.c_str(), 1);
		} else {
			unsetenv("LOTUS_WINDOW_PLATFORM");
		}
	}

private:
	bool had_old_{false};
	std::string old_;
};

/// Sets a signal's handler for a scope and puts the old one back.
class SignalDisposition {
public:
	SignalDisposition(int number, void (*handler)(int))
	        : number_{number},
	          previous_{std::signal(number, handler)}
	{
	}
	~SignalDisposition() { std::signal(number_, previous_); }

	SignalDisposition(const SignalDisposition&)            = delete;
	SignalDisposition& operator=(const SignalDisposition&) = delete;

private:
	int number_;
	void (*previous_)(int);
};

class WindowTest : public ::testing::Test {
protected:
	WindowTest() : platform_{"null"} {}

	void SetUp() override
	{
		window_ = std::make_unique<Window>(64, 48, "window-test");
	}

	void TearDown() override { window_.reset(); }

	/// Everything the window has queued, delivered the way the application
	/// loop receives it.
	std::vector<Event> poll()
	{
		std::vector<Event> events;
		window_->poll_events(
		        [&events](const Event& event) { events.push_back(event); });
		return events;
	}

	PlatformEnv platform_;
	std::unique_ptr<Window> window_;
};

} // namespace

TEST_F(WindowTest, KeyPressUpdatesInputAndIsDelivered)
{
	WindowTestAccess::key(*window_, GLFW_KEY_W, GLFW_PRESS);
	EXPECT_TRUE(window_->input().key_down(Key::W));

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 1u);
	const auto* key = std::get_if<KeyEvent>(&events[0]);
	ASSERT_NE(key, nullptr);
	EXPECT_EQ(key->key, Key::W);
	EXPECT_TRUE(key->pressed);
	EXPECT_FALSE(key->repeat);

	WindowTestAccess::key(*window_, GLFW_KEY_W, GLFW_RELEASE);
	EXPECT_FALSE(window_->input().key_down(Key::W));
}

TEST_F(WindowTest, RepeatKeepsTheKeyDownAndIsFlagged)
{
	WindowTestAccess::key(*window_, GLFW_KEY_A, GLFW_PRESS);
	WindowTestAccess::key(*window_, GLFW_KEY_A, GLFW_REPEAT);
	EXPECT_TRUE(window_->input().key_down(Key::A));

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 2u);
	const auto* repeat = std::get_if<KeyEvent>(&events[1]);
	ASSERT_NE(repeat, nullptr);
	EXPECT_TRUE(repeat->pressed);
	EXPECT_TRUE(repeat->repeat);
}

TEST_F(WindowTest, ModifiersTravelWithTheKey)
{
	WindowTestAccess::key(*window_, GLFW_KEY_Z, GLFW_PRESS,
	                      GLFW_MOD_CONTROL | GLFW_MOD_CAPS_LOCK);
	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(std::get<KeyEvent>(events[0]).mods, Mod::Ctrl);
}

TEST_F(WindowTest, KeysTheEngineDoesNotNameProduceNoEvent)
{
	WindowTestAccess::key(*window_, GLFW_KEY_UNKNOWN, GLFW_PRESS);
	WindowTestAccess::key(*window_, GLFW_KEY_WORLD_1, GLFW_PRESS);
	EXPECT_TRUE(window_->input().keys().none());
	EXPECT_TRUE(poll().empty());
}

TEST_F(WindowTest, EventsArriveInTheOrderTheyHappened)
{
	WindowTestAccess::key(*window_, GLFW_KEY_A, GLFW_PRESS);
	WindowTestAccess::key(*window_, GLFW_KEY_B, GLFW_PRESS);
	WindowTestAccess::key(*window_, GLFW_KEY_A, GLFW_RELEASE);

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 3u);
	const auto& first  = std::get<KeyEvent>(events[0]);
	const auto& second = std::get<KeyEvent>(events[1]);
	const auto& third  = std::get<KeyEvent>(events[2]);
	EXPECT_EQ(first.key, Key::A);
	EXPECT_TRUE(first.pressed);
	EXPECT_EQ(second.key, Key::B);
	EXPECT_TRUE(second.pressed);
	EXPECT_EQ(third.key, Key::A);
	EXPECT_FALSE(third.pressed);
	// A batch is delivered once.
	EXPECT_TRUE(poll().empty());
}

TEST_F(WindowTest, CursorMovesCarryDeltasFromThePreviousPosition)
{
	WindowTestAccess::cursor(*window_, 10.0, 20.0);
	WindowTestAccess::cursor(*window_, 13.0, 18.0);

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 2u);
	const auto& first  = std::get<MouseMoveEvent>(events[0]);
	const auto& second = std::get<MouseMoveEvent>(events[1]);
	// Nothing to measure the first position against.
	EXPECT_FLOAT_EQ(first.dx, 0.0f);
	EXPECT_FLOAT_EQ(first.dy, 0.0f);
	EXPECT_FLOAT_EQ(second.x, 13.0f);
	EXPECT_FLOAT_EQ(second.y, 18.0f);
	EXPECT_FLOAT_EQ(second.dx, 3.0f);
	EXPECT_FLOAT_EQ(second.dy, -2.0f);
	EXPECT_FLOAT_EQ(window_->input().mouse_x(), 13.0f);
}

// The cursor can come back anywhere; a drag that starts there must not begin
// with a jump from where the cursor left.
TEST_F(WindowTest, ReenteringRestartsTheDelta)
{
	WindowTestAccess::cursor(*window_, 10.0, 10.0);
	WindowTestAccess::enter(*window_, false);
	WindowTestAccess::enter(*window_, true);
	WindowTestAccess::cursor(*window_, 400.0, 300.0);

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 2u);
	const auto& back = std::get<MouseMoveEvent>(events[1]);
	EXPECT_FLOAT_EQ(back.dx, 0.0f);
	EXPECT_FLOAT_EQ(back.dy, 0.0f);
}

TEST_F(WindowTest, ADragReportsTheHeldButton)
{
	WindowTestAccess::button(*window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
	EXPECT_TRUE(window_->input().mouse_down(MouseButton::Right));
	WindowTestAccess::cursor(*window_, 5.0, 5.0);

	const std::vector<Event> events = poll();
	ASSERT_EQ(events.size(), 2u);
	const auto& button = std::get<MouseButtonEvent>(events[0]);
	EXPECT_EQ(button.button, MouseButton::Right);
	EXPECT_TRUE(button.pressed);
	const auto& move = std::get<MouseMoveEvent>(events[1]);
	EXPECT_TRUE(move.down(MouseButton::Right));
	EXPECT_FALSE(move.down(MouseButton::Left));

	WindowTestAccess::button(*window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE);
	EXPECT_FALSE(window_->input().mouse_down(MouseButton::Right));
}

TEST_F(WindowTest, ExtraMouseButtonsAreIgnored)
{
	WindowTestAccess::button(*window_, GLFW_MOUSE_BUTTON_4, GLFW_PRESS);
	EXPECT_EQ(window_->input().mouse_buttons(), 0);
	EXPECT_TRUE(poll().empty());
}

TEST_F(WindowTest, CloseRequestEndsTheLoop)
{
	EXPECT_FALSE(window_->should_close());
	WindowTestAccess::close(*window_);
	EXPECT_TRUE(window_->should_close());
}

TEST_F(WindowTest, PixelSizeIsTheFramebufferSize)
{
	int width  = 0;
	int height = 0;
	window_->pixel_size(width, height);
	EXPECT_EQ(width, 64);
	EXPECT_EQ(height, 48);
}

TEST(WindowPlatformTest, AnUnknownPlatformNameIsRejectedClearly)
{
	const PlatformEnv bogus{"carrier-pigeon"};
	try {
		const Window window{64, 48, "never"};
		FAIL() << "a window opened on an unknown platform";
	} catch (const std::runtime_error& error) {
		EXPECT_NE(std::string{error.what()}.find("LOTUS_WINDOW_PLATFORM"),
		          std::string::npos)
		        << error.what();
	}
	// The failed attempt left nothing behind: a valid platform still works.
	const PlatformEnv none{"null"};
	const Window window{64, 48, "after"};
	EXPECT_FALSE(window.should_close());
}

// Ctrl-C and `kill` must end the application in order, so the shutdown code
// runs. A harness may start the tests with a signal ignored, hence each
// round begins from the default; the second round also proves a new window
// does not inherit the first one's request.
TEST(WindowSignalTest, TerminationSignalsRequestAClose)
{
	const PlatformEnv platform{"null"};
	for (const int number : {SIGINT, SIGTERM}) {
		const SignalDisposition fresh{number, SIG_DFL};
		Window window{64, 48, "signal-test"};
		window.poll_events();
		EXPECT_FALSE(window.should_close()) << "signal " << number;
		std::raise(number);
		window.poll_events();
		EXPECT_TRUE(window.should_close()) << "signal " << number;
	}
}

// A signal the process set up itself is not the window's to take.
TEST(WindowSignalTest, ASignalTheProcessIgnoresStaysIgnored)
{
	const PlatformEnv platform{"null"};
	const SignalDisposition ignored{SIGTERM, SIG_IGN};
	Window window{64, 48, "signal-test"};
	std::raise(SIGTERM);
	window.poll_events();
	EXPECT_FALSE(window.should_close());
}

TEST(WindowSignalTest, TheHandlerOutlivesAllButTheLastWindow)
{
	const PlatformEnv platform{"null"};
	const SignalDisposition fresh{SIGTERM, SIG_DFL};
	{
		Window first{64, 48, "first"};
		{
			const Window second{64, 48, "second"};
		}
		std::raise(SIGTERM);
		first.poll_events();
		EXPECT_TRUE(first.should_close());
	}
	// No window left: SIGTERM is back to what it was before the first one.
	EXPECT_EQ(std::signal(SIGTERM, SIG_DFL), SIG_DFL);
}

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lotus {

enum class LogLevel { Info, Warning, Error };

struct ConsoleEntry {
	LogLevel level{LogLevel::Info};
	std::string text;
};

/// Developer console: a capped log plus a registry of named commands.
///
/// Commands take whitespace-separated arguments (no quoting) and return the
/// text to display. Executing a line records both the input and the result,
/// so the console doubles as a transcript.
class Console {
public:
	using CommandFn =
	        std::function<std::string(const std::vector<std::string>& args)>;

	explicit Console(std::size_t capacity = 512);

	void log(LogLevel level, std::string text);
	void info(std::string text) { log(LogLevel::Info, std::move(text)); }
	void warn(std::string text) { log(LogLevel::Warning, std::move(text)); }
	void error(std::string text) { log(LogLevel::Error, std::move(text)); }

	void add_command(std::string name, std::string help, CommandFn fn);
	bool has_command(const std::string& name) const;

	/// Runs a command line, logging the input and its result. Returns the
	/// result text (also recorded in the log).
	std::string execute(const std::string& line);

	const std::deque<ConsoleEntry>& entries() const { return entries_; }
	void clear() { entries_.clear(); }

	/// Registered commands as (name, help), sorted by name.
	std::vector<std::pair<std::string, std::string>> commands() const;

	/// Optional mirror called for every entry (e.g. to echo to stdout).
	using SinkFn = std::function<void(const ConsoleEntry&)>;
	void set_sink(SinkFn sink) { sink_ = std::move(sink); }

	std::size_t capacity() const { return capacity_; }

private:
	struct Command {
		std::string help;
		CommandFn fn;
	};

	std::size_t capacity_;
	std::deque<ConsoleEntry> entries_;
	std::map<std::string, Command> commands_;
	SinkFn sink_;
};

} // namespace lotus

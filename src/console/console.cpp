#include "lotus/console/console.hpp"

#include <sstream>

namespace lotus {
namespace {

std::vector<std::string> tokenize(const std::string& line)
{
	std::vector<std::string> tokens;
	std::istringstream stream{line};
	std::string token;
	while (stream >> token) {
		tokens.push_back(token);
	}
	return tokens;
}

} // namespace

Console::Console(std::size_t capacity) : capacity_{capacity < 1 ? 1 : capacity} {}

void Console::log(LogLevel level, std::string text)
{
	entries_.push_back(ConsoleEntry{level, std::move(text)});
	while (entries_.size() > capacity_) {
		entries_.pop_front();
	}
	if (sink_) {
		sink_(entries_.back());
	}
}

void Console::add_command(std::string name, std::string help, CommandFn fn)
{
	commands_[std::move(name)] = Command{std::move(help), std::move(fn)};
}

bool Console::has_command(const std::string& name) const
{
	return commands_.find(name) != commands_.end();
}

std::vector<std::pair<std::string, std::string>> Console::commands() const
{
	std::vector<std::pair<std::string, std::string>> out;
	out.reserve(commands_.size());
	for (const auto& [name, command] : commands_) {
		out.emplace_back(name, command.help);
	}
	return out;
}

std::string Console::execute(const std::string& line)
{
	const std::vector<std::string> tokens = tokenize(line);
	if (tokens.empty()) {
		return {};
	}

	info("> " + line);

	const auto it = commands_.find(tokens[0]);
	if (it == commands_.end()) {
		const std::string message = "unknown command: " + tokens[0];
		error(message);
		return message;
	}

	std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
	std::string result;
	try {
		result = it->second.fn(arguments);
	} catch (const std::exception& ex) {
		result = std::string{"command failed: "} + ex.what();
		error(result);
		return result;
	}
	if (!result.empty()) {
		info(result);
	}
	return result;
}

} // namespace lotus

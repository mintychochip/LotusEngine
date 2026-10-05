#include <gtest/gtest.h>

#include <stdexcept>

#include "lotus/console/console.hpp"

using namespace lotus;

TEST(ConsoleTest, ExecutesRegisteredCommand)
{
	Console console;
	console.add_command("add", "adds two numbers",
	                    [](const std::vector<std::string>& args) {
		                    if (args.size() != 2) {
			                    return std::string{"usage: add A B"};
		                    }
		                    const int sum = std::stoi(args[0]) + std::stoi(args[1]);
		                    return "= " + std::to_string(sum);
	                    });

	EXPECT_EQ(console.execute("add 2 3"), "= 5");
	EXPECT_TRUE(console.has_command("add"));
	EXPECT_FALSE(console.has_command("nope"));
}

TEST(ConsoleTest, UnknownCommandIsReportedAndLogged)
{
	Console console;
	const std::string result = console.execute("bogus arg");
	EXPECT_NE(result.find("unknown command"), std::string::npos);

	ASSERT_GE(console.entries().size(), 2u);
	EXPECT_EQ(console.entries().back().level, LogLevel::Error);
}

TEST(ConsoleTest, TranscriptRecordsInputAndOutput)
{
	Console console;
	console.add_command("echo", "echoes", [](const std::vector<std::string>& a) {
		return a.empty() ? std::string{} : a[0];
	});
	console.execute("echo hi");

	ASSERT_EQ(console.entries().size(), 2u);
	EXPECT_EQ(console.entries()[0].text, "> echo hi");
	EXPECT_EQ(console.entries()[1].text, "hi");
}

TEST(ConsoleTest, EmptyLineIsIgnored)
{
	Console console;
	EXPECT_TRUE(console.execute("   ").empty());
	EXPECT_TRUE(console.entries().empty());
}

TEST(ConsoleTest, LogRingIsCapped)
{
	Console console{3};
	for (int i = 0; i < 10; ++i) {
		console.info("line " + std::to_string(i));
	}
	ASSERT_EQ(console.entries().size(), 3u);
	EXPECT_EQ(console.entries().front().text, "line 7");
	EXPECT_EQ(console.entries().back().text, "line 9");
}

TEST(ConsoleTest, ClearEmptiesLog)
{
	Console console;
	console.info("something");
	console.clear();
	EXPECT_TRUE(console.entries().empty());
}

TEST(ConsoleTest, CommandThatThrowsIsCaught)
{
	Console console;
	console.add_command(
	        "boom", "throws",
	        [](const std::vector<std::string>&) -> std::string {
		        throw std::runtime_error("kaboom");
	        });
	const std::string result = console.execute("boom");
	EXPECT_NE(result.find("command failed"), std::string::npos);
	EXPECT_NE(result.find("kaboom"), std::string::npos);
}

TEST(ConsoleTest, CommandsAreListedSorted)
{
	Console console;
	console.add_command("zeta", "", [](const std::vector<std::string>&) { return ""; });
	console.add_command("alpha", "", [](const std::vector<std::string>&) { return ""; });
	const auto commands = console.commands();
	ASSERT_EQ(commands.size(), 2u);
	EXPECT_EQ(commands[0].first, "alpha");
	EXPECT_EQ(commands[1].first, "zeta");
}

#include <iostream>

#include "lotus/rpg/game.hpp"

int main()
{
	try {
		lotus::rpg::RPGGame app;
		app.run(1100, 700, "Lotus RPG");
	} catch (const std::exception& ex) {
		std::cerr << "lotus rpg: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}

all: lotus

lotus: main.cc
	g++ main.cc -g -o lotus

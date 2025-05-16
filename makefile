all: lotus

lotus: main.cc
	g++ -Icore main.cc -g -o lotus

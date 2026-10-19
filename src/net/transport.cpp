#include "lotus/net/transport.hpp"

using namespace lotus;

bool Endpoint::operator==(const Endpoint& other) const {
	return address == other.address && port == other.port;
}

#include "lotus/net/udp_transport.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lotus/net/protocol.hpp"

namespace lotus {
namespace {

constexpr int kMaxDatagramsPerPoll    = 256;
constexpr std::size_t kReceiveScratch = 2048;

} // namespace

UdpTransport::UdpTransport() = default;

UdpTransport::~UdpTransport() { close(); }

bool UdpTransport::bind(std::uint16_t port)
{
	close();
	fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd_ < 0) {
		return false;
	}

	int reuse = 1;
	::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(port);

	if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
		close();
		return false;
	}

	int flags = ::fcntl(fd_, F_GETFL, 0);
	if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
		close();
		return false;
	}

	sockaddr_in bound{};
	socklen_t len = sizeof(bound);
	if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
		close();
		return false;
	}
	local_.address = "127.0.0.1";
	local_.port    = ntohs(bound.sin_port);
	bound_         = true;
	return true;
}

void UdpTransport::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
	bound_ = false;
}

bool UdpTransport::send(const Endpoint& to,
                        std::span<const std::uint8_t> bytes)
{
	if (fd_ < 0 || bytes.size() > kMtu || to.address.empty()) {
		return false;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(to.port);
	if (::inet_pton(AF_INET, to.address.c_str(), &addr.sin_addr) != 1) {
		return false;
	}
	const ssize_t sent = ::sendto(fd_,
	                              bytes.data(),
	                              bytes.size(),
	                              0,
	                              reinterpret_cast<sockaddr*>(&addr),
	                              sizeof(addr));
	return sent == static_cast<ssize_t>(bytes.size());
}

std::vector<Datagram> UdpTransport::poll()
{
	std::vector<Datagram> out;
	if (fd_ < 0) {
		return out;
	}
	for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
		std::uint8_t scratch[kReceiveScratch];
		sockaddr_in from{};
		socklen_t fromlen = sizeof(from);
		const ssize_t n   = ::recvfrom(fd_,
		                             scratch,
		                             sizeof(scratch),
		                             0,
		                             reinterpret_cast<sockaddr*>(&from),
		                             &fromlen);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (static_cast<std::size_t>(n) > kMtu) {
			continue;
		}
		Datagram d;
		d.bytes.assign(scratch, scratch + n);
		char ip[INET_ADDRSTRLEN] = {};
		if (::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)) == nullptr) {
			continue;
		}
		d.from.address = ip;
		d.from.port    = ntohs(from.sin_port);
		out.push_back(std::move(d));
	}
	return out;
}

std::optional<Endpoint> UdpTransport::local_endpoint() const
{
	return bound_ ? std::optional<Endpoint>{local_} : std::nullopt;
}

} // namespace lotus

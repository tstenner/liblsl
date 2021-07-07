#include "socket_utils.h"
#include "api_config.h"
#include "common.h"
#include "loguru.hpp"
#include <boost/endian/conversion.hpp>
#include <chrono>


/// actual computation for the endian performance measurement
int _measure_endian_performance() {
	const auto measure_duration = std::chrono::milliseconds(10);
	const auto t_start = std::chrono::steady_clock::now();

	uint64_t data = 0x01020304;
	int k = 0;
	for (; (k & 0xFFFF) != 0 || std::chrono::steady_clock::now() - t_start < measure_duration; k++)
		lslboost::endian::endian_reverse_inplace(data);

	// time taken in centiseconds (10s/1000)
	std::chrono::duration<double, std::ratio<10, 1000>> benchmark_time =
		std::chrono::steady_clock::now() - t_start;
	LOG_F(INFO, "Endian performance n: %d, per 10ms: %.0f, t: %f ms", k, k / benchmark_time.count(), benchmark_time.count() * 10);
	k = static_cast<int>(k / benchmark_time.count());

	return k;
}

int lsl::measure_endian_performance() {
	static int measured_endian_performance{-1};
	// calculate the endian performance once
	if (measured_endian_performance == -1)
		measured_endian_performance = _measure_endian_performance();
	// return the cached endian performance
	return measured_endian_performance;
}

template <typename Socket, typename Protocol>
uint16_t bind_port_in_range_(Socket &sock, Protocol protocol) {
	const auto *cfg = lsl::api_config::get_instance();
	lslboost::system::error_code ec;
	for (uint16_t port = cfg->base_port(), e = port + cfg->port_range(); port < e; port++) {
		sock.bind(typename Protocol::endpoint(protocol, port), ec);
		if (ec == lslboost::system::errc::address_in_use) continue;
		if (!ec) return port;
	}
	if (cfg->allow_random_ports()) {
		for (int k = 0; k < 100; ++k) {
			uint16_t port = 1025 + rand() % 64000;
			sock.bind(typename Protocol::endpoint(protocol, port), ec);
			if (ec == lslboost::system::errc::address_in_use) continue;
			if (!ec) return port;
		}
	}
	return 0;
}

const std::string all_ports_bound_msg(
	"All local ports were found occupied. You may have more open outlets on this machine than your "
	"PortRange setting allows (see "
	"https://labstreaminglayer.readthedocs.io/info/network-connectivity.html"
	") or you have a problem with your network configuration.");

uint16_t lsl::bind_port_in_range(asio::ip::udp::socket &sock, asio::ip::udp protocol) {
	uint16_t port = bind_port_in_range_(sock, protocol);
	if (!port) throw std::runtime_error(all_ports_bound_msg);
	return port;
}

uint16_t lsl::bind_and_listen_to_port_in_range(
	asio::ip::tcp::acceptor &acc, asio::ip::tcp protocol, int backlog) {
	uint16_t port = bind_port_in_range_(acc, protocol);
	if (!port) throw std::runtime_error(all_ports_bound_msg);
	acc.listen(backlog);
	return port;
}

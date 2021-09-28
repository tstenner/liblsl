#include "resolve_packet_sender.hpp"
#include "api_config.h"
#include "netinterfaces.h"
#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/multicast.hpp>

using asio::ip::multicast::outbound_interface;

static void filter_addrs_by_proto(std::vector<asio::ip::address> &addresses, udp proto) {
	auto end = std::remove_if(addresses.begin(), addresses.end(),
		[proto](const asio::ip::address &addr) { return addr.is_v4() == (proto == udp::v4()); });
	addresses.erase(end, addresses.end());
}

static std::vector<asio::ip::address> addresses_filtered_by_proto(
	std::vector<asio::ip::address> addresses, udp proto) {
	filter_addrs_by_proto(addresses, proto);
	return addresses;
}

void lsl::base_query_sender::send_packet(udp::socket& sock, const udp::endpoint& ep) const
{
	sock.async_send_to(msg_, ep, [](err_t /*unused*/, std::size_t /*unused*/) {});
}

lsl::base_query_sender::~base_query_sender() noexcept = default;

lsl::unicast_query_sender::unicast_query_sender(asio::const_buffer buf, asio::ip::udp proto,
	asio::io_context &ctx, const std::vector<asio::ip::address> &addrs)
	: base_query_sender(buf), addrs_(addresses_filtered_by_proto(addrs, proto)),
	  portrange_begin(api_config::get_instance()->base_port()),
	  portrange_end(portrange_begin + api_config::get_instance()->port_range()), sock_(ctx, proto) {
}

lsl::multicast_query_sender::multicast_query_sender(asio::const_buffer buf, asio::ip::udp proto,
	asio::io_context &ctx, std::vector<asio::ip::address> addrs, uint16_t port)
	: base_query_sender(buf), addrs_(addresses_filtered_by_proto(addrs, proto)), port_(port) {

	// construct an interface with default settings, i.e. let the OS choose
	std::vector<lsl::netif> interfaces({lsl::netif()});
	interfaces[0].addr = asio::ip::address_v4();
	sockets_.reserve(interfaces.size());

	auto opt_ttl = asio::ip::multicast::hops(api_config::get_instance()->multicast_ttl());
	for (const auto &netif : interfaces) {
		udp::socket sock{ctx, proto};
		lslboost::system::error_code ec;
		sock.set_option(opt_ttl, ec);
		if (ec) continue;
		if (proto == udp::v4())
			sock.set_option(outbound_interface(netif.addr.to_v4()), ec);
		else
			sock.set_option(outbound_interface(netif.ifindex), ec);
		if (ec) continue;
		sockets_.emplace_back(std::move(sock));
	}
}

void lsl::unicast_query_sender::send_packets() {
	for (auto port = portrange_begin; port < portrange_end; ++port)
		for (const auto &addr : addrs_) send_packet(sock_, udp::endpoint(addr, port));
}

void lsl::multicast_query_sender::send_packets() {
	for (const auto &addr : addrs_)
		for (auto &sock : sockets_)
			send_packet(sock, udp::endpoint(addr, port_));
}

void lsl::broadcast_query_sender::send_packets() {
	send_packet(sock_, udp::endpoint(asio::ip::address_v4::broadcast(), port_));
}



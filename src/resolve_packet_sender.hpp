#pragma once
#include "forward.h"
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <string>
#include <vector>

using asio::ip::udp;
using err_t = const lslboost::system::error_code &;

namespace lsl {

class base_query_sender {
private:
	asio::const_buffer msg_;
protected:
	void send_packet(udp::socket& sock, const udp::endpoint& ep) const;
public:
	base_query_sender(asio::const_buffer buf) : msg_(buf) {}

	virtual ~base_query_sender() noexcept;
	virtual void send_packets() = 0;
};

class unicast_query_sender : private base_query_sender {
private:
	std::vector<asio::ip::address> addrs_;
	uint16_t portrange_begin, portrange_end;
	udp::socket sock_;

public:
	unicast_query_sender(asio::const_buffer buf, udp proto, asio::io_context &ctx,
		const std::vector<asio::ip::address> &endpoints);

	void send_packets() override;

	~unicast_query_sender() override {
		sock_.close();
		// TODO: wait for async ops to be finished
	}
};

class broadcast_query_sender : private base_query_sender {
private:
	udp::socket sock_;
	uint16_t port_;

public:
	broadcast_query_sender(asio::const_buffer buf, udp proto, asio::io_context &ctx, uint16_t port)
		: base_query_sender(buf), sock_(ctx, proto), port_(port) {
		if (proto == udp::v6()) throw std::invalid_argument("broadcast requested for IPv6");
		sock_.set_option(asio::socket_base::broadcast(true));
	}

	void send_packets() override;

	~broadcast_query_sender() override {
		sock_.close();
		// TODO: wait for async ops to be finished
	}
};

class multicast_query_sender : private base_query_sender {
private:
	std::vector<asio::ip::address> addrs_;
	std::vector<udp::socket> sockets_;
	uint16_t port_;

public:
	multicast_query_sender(asio::const_buffer buf, udp proto, asio::io_context &ctx,
		std::vector<asio::ip::address> addrs, uint16_t port);

	void send_packets() override;

	~multicast_query_sender() override {
		for (auto &sock : sockets_) sock.close();
		// TODO: wait for async ops to be finished
	}
};

} // namespace lsl

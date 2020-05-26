#include "resolve_attempt_udp.h"
#include "api_config.h"
#include "netinterfaces.h"
#include "resolver_impl.h"
#include "socket_utils.h"
#include <algorithm>
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ip/v6_only.hpp>

using namespace lslboost::asio;
using err_t = const lslboost::system::error_code &;
using ip::multicast::outbound_interface;

namespace lsl {
ip::udp::endpoint force_v6_addr(ip::udp::endpoint &ep) {
	if (ep.address().is_v4())
		ep.address(ip::make_address_v6(ip::v4_mapped_t(), ep.address().to_v4()));
	return ep;
}

bool resolve_query_sender::send_query(const std::string &buf) {
	bool any_successful = false;
	error_code ec;
	const_buffer sendbuf(buf.data(), buf.length());
	for (auto ep : targets_) {
		sock_.send_to(sendbuf, ep, 0, ec);
		if (!ec) any_successful = true;
	}
	return any_successful;
}

bool resolve_attempt::is_done() {
	if (cancelled_) return true;
	double now = lsl_clock();
	if (now > cancel_after_) return true;
	if (minimum_ == 0) return false;

	std::lock_guard<std::mutex> lock(results_mut_);
	DLOG_F(1, "Checking result set size (%lu / %d)…", results_.size(), minimum_);
	return results_.size() >= (std::size_t)minimum_ && now >= resolve_atleast_until_;
}

resolve_attempt::resolve_attempt(const endpoint_list &ucast_targets,
	const endpoint_list &mcast_targets, const std::string &query)
	: unicast_targets_(ucast_targets), recv_socket_(io_), cancel_timer_(io_), multicast_timer_(io_),
	  unicast_timer_(io_) {

	auto cfg = api_config::get_instance();
	// Which protocols may we use?
	bool ipv4 = cfg->allow_ipv4(), ipv6 = cfg->allow_ipv6();
	auto protocol = ipv6 ? ip::udp::v6() : ip::udp::v4();
	if (ipv6) {
		try {
			recv_socket_.open(ip::udp::v6());
			// the IPv6 socket can also receive IPv4 replies unless v6_only is set
			// this fails when the OS isn't dual stack capable so we fall back to IPv4 only
			recv_socket_.set_option(ip::v6_only(!ipv4));
		} catch (std::exception &e) {
			LOG_F(WARNING, "Couldn't open IPv6 socket: %s", e.what());
			recv_socket_.close();
			ipv6 = false;
			if (!ipv4) throw std::runtime_error("IPv6 support unavailable and IPv4 disabled");
		}
	}
	if (!recv_socket_.is_open() && ipv4) {
		protocol = ip::udp::v4();
		recv_socket_.open(ip::udp::v4());
	}
	try {
		bind_port_in_range(recv_socket_, protocol);
	} catch (std::exception &e) {
		LOG_F(WARNING,
			"Could not bind to a port in the configured port range; using a randomly assigned one: "
			"%s",
			e.what());
	}

	for (auto &ep : mcast_targets) {
		ip::address addr = ep.address();
		if (!ipv4 && addr.is_v4()) continue;
		if (!ipv6 && addr.is_v6()) continue;
		if (addr.is_multicast()) mcast_targets_[addr.is_v4() ? 0 : 1].push_back(ep);
		// Let's assume this is a valid broadcast address.
		// Otherwise, only one outlet will receive the query.
		else
			broadcast_targets_.push_back(ep);
	}

	// Save the query for later in case validation is enabled
	if (cfg->validate_query_responses()) query_ = query;
	// precalc the query id (hash of the query string, as string)
	query_id_ = std::to_string(std::hash<std::string>()(query));
	// precalc the query message
	query_msg_ = "LSL:shortinfo\r\n";
	query_msg_.append(query).append("\r\n");
	query_msg_.append(std::to_string(recv_socket_.local_endpoint().port()));
	query_msg_.append(" ").append(query_id_).append("\r\n");


	DLOG_F(2, "Waiting for query results (port %d) for %s", recv_socket_.local_endpoint().port(),
		query_msg_.c_str());
	if (!unicast_targets_.empty())
		unicast_sender = std::make_unique<resolve_query_sender>(
			ip::udp::socket(io_, protocol), unicast_targets_);

	if (ipv4 && !broadcast_targets_.empty()) {
		try {
			ip::udp::socket broadcast_socket_(io_, ip::udp::v4());
			broadcast_socket_.set_option(socket_base::broadcast(true));
			resolve_query_sender rqs(std::move(broadcast_socket_), broadcast_targets_);
			if (rqs.send_query(query_msg_)) mcast_senders.emplace_back(std::move(rqs));
		} catch (std::exception &e) {
			LOG_F(WARNING, "Cannot open UDP broadcast socket for resolves: %s", e.what());
		}
	}

	for (int i = 0; i < 1; ++i) {
		if (i == 0 && !ipv4) continue;
		if (i == 1 && !ipv6) continue;
		auto proto = i == 0 ? ip::udp::v4() : ip::udp::v6();
		try {
			{
				ip::udp::socket mcast_sock(io_, proto);
				mcast_sock.set_option(
					ip::multicast::hops(api_config::get_instance()->multicast_ttl()));
				mcast_sock.set_option(ip::multicast::enable_loopback(true));
				error_code ec;
				bool any_join_succeeded = false;
				for (const auto &ep : mcast_targets_[i]) {
					auto addr = ep.address();
					DLOG_F(INFO, "Multicast iter %d addr %s", i, addr.to_string().c_str());
					auto option = i == 0 ? ip::multicast::join_group(addr.to_v4())
										 : ip::multicast::join_group(addr.to_v6());
					mcast_sock.set_option(option, ec);
					if (ec)
						LOG_F(WARNING, "Cannot join multicast group %s: %s",
							addr.to_string().c_str(), ec.message().c_str());
					else
						any_join_succeeded = true;
				}
				if (any_join_succeeded)
					mcast_senders.emplace_back(std::move(mcast_sock), mcast_targets_[i]);
			}
		} catch (std::exception &e) {
			LOG_F(WARNING, "Cannot open UDP multicast socket for resolves: %s", e.what());
		}
	}
}

void resolve_attempt::setup_handlers(
	double unicast_wait, double multicast_wait, double cancel_after) {
	// initiate the result gathering chain
	receive_next_result();

	// initiate the cancel event, if desired
	cancel_after_ = lsl_clock() + cancel_after;
	if (cancel_after < FOREVER) {
		cancel_timer_.expires_after(timeout_sec(cancel_after));
		cancel_timer_.async_wait([shared_this = shared_from_this(), this](err_t err) {
			DLOG_F(1, "resolve_attempt cancelled %d %s", err.value(), err.message().c_str());
			if (!err) do_cancel();
		});
	}

	if (unicast_sender) {
		schedule_packet_burst(unicast_timer_, unicast_wait, *unicast_sender);
		// delay the next multicast wave by unicast_rtt
		multicast_wait += api_config::get_instance()->unicast_min_rtt();
		unicast_sender->send_query(query_msg_);
	}
	for (auto &sender : mcast_senders) {
		schedule_packet_burst(multicast_timer_, multicast_wait, sender);
		sender.send_query(query_msg_);
	}
}

resolve_attempt::~resolve_attempt() {
	DLOG_F(2, "Destructor called for resolve_attempt");
	if (std::any_of(mcast_senders.begin(), mcast_senders.end(),
			[](const resolve_query_sender &rqs) { return rqs.is_open(); }) ||
		recv_socket_.is_open())
		LOG_F(ERROR, "destructor called for running resolve_attempt");
}

void resolve_attempt::cancel() {
	post(io_, [shared_this = shared_from_this()]() { shared_this->do_cancel(); });
}


// === receive loop ===
void resolve_attempt::receive_next_result() {
	recv_socket_.async_receive_from(buffer(resultbuf_), remote_endpoint_,
		[shared_this = shared_from_this()](
			err_t err, size_t len) { shared_this->handle_receive_outcome(err, len); });
}

void resolve_attempt::schedule_packet_burst(
	resolve_attempt::timer_t &timer, double delay, resolve_query_sender &sender) {
	if (cancelled_) return;

	timer.expires_after(timeout_sec(delay));
	timer.async_wait([&timer, delay, &sender, this](err_t err) {
		if (err != error::operation_aborted) {
			sender.send_query(query_msg_);
			schedule_packet_burst(timer, delay, sender);
		}
	});
}

void resolve_attempt::handle_receive_outcome(error_code err, std::size_t len) {
	if (cancelled_ || err == error::operation_aborted || err == error::not_connected ||
		err == error::not_socket)
		return;

	if (!err) {
		try {
			// first parse & check the query id
			auto first_newline = std::find(resultbuf_, resultbuf_ + len, '\n');
			if (first_newline == resultbuf_ + len)
				throw std::runtime_error("no data after newline");
			std::string returned_id(trim(std::string(resultbuf_, first_newline)));

			if (returned_id == query_id_) {
				// parse the rest of the query into a stream_info
				stream_info_impl info;
				info.from_shortinfo_message(std::string(first_newline, resultbuf_ + len));
				if (!query_.empty() && !info.matches_query(query_, true))
					throw std::runtime_error("Received streaminfo doesn't match the query");
				std::string uid = info.uid();

				// update the results
				std::lock_guard<std::mutex> lock(results_mut_);
				if (results_.find(uid) == results_.end())
					results_[uid] = std::make_pair(info, lsl_clock()); // insert new result
				else
					results_[uid].second = lsl_clock(); // update only the receive time
				// ... also update the address associated with the result (but don't
				// override the address of an earlier record for this stream since this
				// would be the faster route)
				if (remote_endpoint_.address().is_v4()) {
					if (results_[uid].first.v4address().empty())
						results_[uid].first.v4address(remote_endpoint_.address().to_string());
				} else {
					if (results_[uid].first.v6address().empty())
						results_[uid].first.v6address(remote_endpoint_.address().to_string());
				}
			}
		} catch (std::exception &e) {
			LOG_F(WARNING, "resolve_attempt: hiccup while processing the received data: %s",
				e.what());
		}
	}

	// ask for the next result when there's more queries to find
	if (is_done())
		do_cancel();
	else
		receive_next_result();
}

void resolve_attempt::do_cancel() {
	try {
		cancelled_ = true;
		cancel_timer_.cancel();
		unicast_timer_.cancel();
		multicast_timer_.cancel();
		for (auto &sender : mcast_senders) sender.close();
		if (recv_socket_.is_open()) recv_socket_.close();
	} catch (std::exception &e) {
		LOG_F(WARNING, "Unexpected error while trying to cancel operations of resolve_attempt: %s",
			e.what());
	}
}

} // namespace lsl

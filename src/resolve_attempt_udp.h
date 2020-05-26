#pragma once
#include "cancellation.h"
#include "netinterfaces.h"
#include "stream_info_impl.h"
#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <map>

using namespace lslboost::asio;
using endpoint_list = std::vector<ip::udp::endpoint>;
using lslboost::system::error_code;
using time_point_t = std::chrono::steady_clock::time_point;
using duration_t = std::chrono::steady_clock::duration;

namespace pugi {
class xpath_query;
}

namespace lsl {
/// A container for resolve results (map from stream instance UID onto (stream_info,receive-time)).
typedef std::map<std::string, std::pair<stream_info_impl, double>> result_container;
/// A container for outgoing multicast interfaces
typedef std::vector<class netif> mcast_interface_list;

class resolve_query_sender {
	ip::udp::socket sock_;
	const endpoint_list &targets_;
public:
	resolve_query_sender(ip::udp::socket &&sock, const endpoint_list &targets)
		: sock_(std::move(sock)), targets_(targets) {}
	resolve_query_sender(resolve_query_sender &&) = default;
	~resolve_query_sender() = default;
	void close() { sock_.close(); }
	bool is_open() const { return sock_.is_open(); }

	bool send_query(const std::string &buf);
};

/**
 * An asynchronous resolve attempt for a single query targeted at a set of endpoints, via UDP.
 *
 * A resolve attempt is an asynchronous operation submitted to an IO object, which amounts to a
 * sequence of query packet sends (one for each endpoint in the list) and a sequence of result
 * packet receives. The operation will wait for return packets until either a particular timeout has
 * been reached or until it is cancelled via the cancel() method.
 */
class resolve_attempt : public std::enable_shared_from_this<resolve_attempt> {
	using timer_t = lslboost::asio::steady_timer;
public:
	/**
	 * Instantiate and set up a new resolve attempt.
	 *
	 * @param io The io_context that will run the async operations.
	 * @param protocol The protocol (either udp::v4() or udp::v6()) to use for communications;
	 * only the subset of target addresses matching this protocol will be considered.
	 * @param targets A list of udp::endpoint that should be targeted by this query.
	 * @param query The query string to send (usually a set of conditions on the properties of the
	 * stream info that should be searched, for example `name='BioSemi' and type='EEG'`.
	 * See lsl_stream_info_matches_query for the definition of a query.
	 * @param results Reference to a container into which results are stored; potentially shared
	 * with other parallel resolve operations. Since this is not thread-safe all operations
	 * modifying this must run on the same single-threaded IO service.
	 * @param results_mut Reference to a mutex that protects the container.
	 * @param cancel_after The time duration after which the attempt is automatically cancelled,
	 * i.e. the receives are ended.
	 * @param registry A registry where the attempt can register itself as active so it can be
	 * cancelled during shutdown.
	 */
	resolve_attempt(const endpoint_list &ucast_targets,
		const endpoint_list &mcast_targets, const std::string &query);
	/// Destructor
	~resolve_attempt();

	/// Set up handlers and timers. This has to be called after the constructor
	/// is finished because otherwise enable_shared_from_this() will fail.
	void setup_handlers(double unicast_wait, double multicast_wait, double quit_after = FOREVER);

	/**
	 * Cancel operations asynchronously, and destructively.
	 * @note This mostly serves to expedite the destruction of the object, which would happen anyway
	 * after some time. As the attempt instance is owned by the handler chains the cancellation
	 * eventually leads to the destruction of the object.
	 */
	void cancel();
private:
	bool is_done();

	/// @section send and receive handlers ===
	friend class resolver_impl;

	/// This function asks to receive the next result packet.
	void receive_next_result();

	/// Schedule sending the next queries. Schedules itself again after the timer expires.
	void schedule_packet_burst(timer_t& timer, double delay, resolve_query_sender& sender);

	/// Handler that gets called when a receive has completed.
	void handle_receive_outcome(error_code err, std::size_t len);

	// === cancellation ===
	/// Cancel the outstanding operations, has to be called from an io_context
	void do_cancel();

	// data shared with the resolver_impl
	/// the IO context that executes our actions
	lslboost::asio::io_context io_;
	/// shared result container
	result_container results_;
	/// shared mutex that protects the results
	std::mutex results_mut_;

	// constant over the lifetime of this attempt
	/// the timepoint after which we're giving up
	double cancel_after_;
	/// whether the operation has been cancelled
	bool cancelled_{false};
	endpoint_list unicast_targets_;
	endpoint_list broadcast_targets_;
	/// list of endpoints that should receive the query (mcast v4, mcast v6)
	endpoint_list mcast_targets_[2];
	/// optional: the query XPath
	std::string query_;
	/// the query message that we're sending
	std::string query_msg_;
	/// the (more or less) unique id for this query
	std::string query_id_;
	/// the minimum number of results that we want
	int minimum_{0};
	/// wait until this point in time before returning results (optional to allow for returning
	/// potentially more than a minimum number of results)
	double resolve_atleast_until_{0};

	// data maintained/modified across handler invocations
	/// the endpoint from which we received the last result
	ip::udp::endpoint remote_endpoint_;
	/// holds a single result received from the net
	char resultbuf_[65536];

	// IO objects
	/// socket to receive replies (always unicast)
	ip::udp::socket recv_socket_;
	/// timer to schedule the cancel action
	timer_t cancel_timer_;
	/// Optional: socket to send unicast queries from
	std::unique_ptr<resolve_query_sender> unicast_sender;
	/// sockets the queries are sent from
	std::vector<resolve_query_sender> mcast_senders;
	/// a timer that fires when a new wave should be scheduled
	timer_t multicast_timer_;
	/// a timer that fires when the unicast wave should be scheduled
	timer_t unicast_timer_;
};
} // namespace lsl

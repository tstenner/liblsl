#include "resolver_impl.h"
#include "api_config.h"
#include "resolve_attempt_udp.h"
#include "socket_utils.h"
#include <boost/asio/ip/udp.hpp>
#include <memory>
#include <thread>


// === implementation of the resolver_impl class ===

using namespace lsl;
using namespace lslboost::asio;
using err_t = const lslboost::system::error_code &;

resolver_impl::resolver_impl() {
	auto cfg_ = api_config::get_instance();
	// parse the multicast addresses into endpoints and store them
	uint16_t mcast_port = cfg_->multicast_port();
	for (const auto &mcast_addr : cfg_->multicast_addresses()) {
		try {
			mcast_endpoints_.emplace_back(mcast_addr, mcast_port);
		} catch (std::exception &) {}
	}

	io_context ctx;
	// parse the per-host addresses into endpoints, and store them, too
	udp::resolver udp_resolver(ctx);
	// for each known peer...
	for (const auto &peer : cfg_->known_peers()) {
		try {
			// resolve the name
			// for each endpoint...
			for (auto &res : udp_resolver.resolve(peer, std::to_string(cfg_->base_port()))) {
				// for each port in the range...
				for (int p = cfg_->base_port(); p < cfg_->base_port() + cfg_->port_range(); p++)
					// add a record
					ucast_endpoints_.emplace_back(res.endpoint().address(), p);
			}
		} catch (std::exception &) {}
	}
}

void check_query(const std::string &query) {
	try {
		pugi::xpath_query(query.c_str());
	} catch (std::exception &e) {
		throw std::invalid_argument((("Invalid query '" + query) += "': ") += e.what());
	}
}

std::string resolver_impl::build_query(const char *pred_or_prop, const char *value) {
	std::string query("session_id='");
	query += api_config::get_instance()->session_id();
	query += '\'';
	if (pred_or_prop) (query += " and ") += pred_or_prop;
	if (value) ((query += "='") += value) += '\'';
	return query;
}

resolver_impl *resolver_impl::create_resolver(
	double forget_after, const char *pred_or_prop, const char *value) noexcept {
	try {
		auto *resolver = new resolver_impl();
		resolver->resolve_continuous(build_query(pred_or_prop, value), forget_after);
		return resolver;
	} catch (std::exception &e) {
		LOG_F(ERROR, "Error while creating a continuous_resolver: %s", e.what());
		return nullptr;
	}
}

// === resolve functions ===

std::vector<stream_info_impl> resolver_impl::resolve_oneshot(
	const std::string &query, int minimum, double timeout, double minimum_time) {
	if (background_io_)
		throw std::invalid_argument("Resolver is already running in continuous mode");
	check_query(query);

	current_resolve = create_attempt(query);
	resolve_attempt &at = *current_resolve;

	// reset the query parameters
	at.minimum_ = minimum;
	at.resolve_atleast_until_ = lsl_clock() + minimum_time;

	auto cfg_ = api_config::get_instance();
	current_resolve->setup_handlers(cfg_->unicast_min_rtt(), cfg_->multicast_min_rtt(), timeout);

	std::vector<stream_info_impl> output;
	if (!cancelled_) {
		// run the IO operations until finished
		at.io_.run();
		// collect output
		// After the io_context is finished, we're the only thread accessing the results so we don't
		// need a mutex
		output.reserve(at.results_.size());
		for (auto &result : at.results_) output.emplace_back(std::move(result.second.first));
	}
	return output;
}

void resolver_impl::resolve_continuous(const std::string &query, double forget_after) {
	if (background_io_)
		throw std::invalid_argument("Resolver is already running in continuous mode");
	check_query(query);
	// reset the IO service & set up the query parameters
	forget_after_ = forget_after;
	//expired_ = false;
	current_resolve = create_attempt(query);
	resolve_attempt &at = *current_resolve;
	auto cfg_ = api_config::get_instance();
	at.setup_handlers(cfg_->unicast_min_rtt() + cfg_->continuous_resolve_interval(),
					  cfg_->multicast_min_rtt() + cfg_->continuous_resolve_interval());
	// spawn a thread that runs the IO operations
	background_io_ = std::make_shared<std::thread>([attempt = current_resolve]() { attempt->io_.run(); });
}

std::vector<stream_info_impl> resolver_impl::results(uint32_t max_results) {
	if(!current_resolve) throw std::logic_error("No ongoing continuous_resolve");
	std::vector<stream_info_impl> output;
	std::lock_guard<std::mutex> lock(current_resolve->results_mut_);
	double expired_before = lsl_clock() - forget_after_;
	auto& res = current_resolve->results_;

	for (auto it = res.begin(); it != res.end();) {
		if (it->second.second < expired_before)
			it = res.erase(it);
		else {
			if (output.size() < max_results) output.push_back(it->second.first);
			it++;
		}
	}
	return output;
}

// === timer-driven async handlers ===

std::shared_ptr<resolve_attempt> resolver_impl::create_attempt(const std::string& query)
{
	return std::make_shared<resolve_attempt>(ucast_endpoints_, mcast_endpoints_, query);
}

// === cancellation and teardown ===

void resolver_impl::cancel() {
	cancelled_ = true;
	if(current_resolve) current_resolve->cancel();
}

resolver_impl::~resolver_impl() {
	try {
		cancel();
		if (background_io_) background_io_->join();
	} catch (std::exception &e) {
		LOG_F(WARNING, "Error during destruction of a resolver_impl: %s", e.what());
	} catch (...) { LOG_F(ERROR, "Severe error during destruction of a resolver_impl."); }
}

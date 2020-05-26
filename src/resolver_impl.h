#ifndef RESOLVER_IMPL_H
#define RESOLVER_IMPL_H

#include "cancellation.h"
#include "common.h"
#include "forward.h"
#include "stream_info_impl.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <mutex>
#include <thread>

using namespace lslboost::asio;
using lslboost::system::error_code;
using endpoint_list = std::vector<ip::udp::endpoint>;
using lslboost::system::error_code;

namespace lsl {
class api_config;

/**
 * A stream resolver object.
 *
 * Maintains the necessary resources for a resolve process,
 * used by the free-standing resolve functions, the continuous_resolver class, and the inlets.
 *
 * A resolver instance can be operated in two different ways:
 *
 * 1) In one shot: The resolver is queried one or more times by calling resolve_oneshot().
 *
 * 2) Continuously: First a background query process is started that keeps updating a results list
 * by calling resolve_continuous() and the list is retrieved in parallel when desired via results().
 * In this case a new resolver instance must be created to issue a new query.
 */
class resolver_impl : public cancellable_registry {
public:
	/**
	 * Instantiate a new resolver and configure timing parameters.
	 *
	 * @note Resolution logic: If api_config::known_peers is empty, a new multicast wave will be
	 * scheduled every mcast_min_rtt (until a timeout expires or the desired number of streams has
	 * been resolved).
	 * If api_config::known_peers is non-empty, a multicast wave and a unicast wave
	 * will be scheduled in alternation.
	 * The spacing between waves will be no shorter than the respective minimum RTTs.
	 * In continuous mode a special, somewhat more lax, set of timings is used (see API config).
	 */
	resolver_impl();

	/**
	 * Build a query string
	 *
	 * @param pred_or_prop an entire predicate if value isn't set or the name of the property, e.g.
	 * `foo='bar'` / `foo` (+value set as "bar")
	 * @param value the value for the property parameter
	 */
	static std::string build_query(const char *pred_or_prop = nullptr, const char *value = nullptr);

	/**
	 * Create a resolver object with optionally a predicate or property + value
	 *
	 * @param forget_after Seconds since last response after which a stream isn't considered to be
	 * online any more.
	 * @param pred_or_prop an entire predicate of value isn't set or the name of the property, e.g.
	 * `foo='bar'` / `foo` (+value set as "bar")
	 * @param value the value for the property parameter
	 * @return A pointer to the resolver on success or nullptr on error
	 */
	static resolver_impl *create_resolver(double forget_after, const char *pred_or_prop = nullptr,
		const char *value = nullptr) noexcept;

	/// Destructor. Cancels any ongoing processes and waits until they finish.
	~resolver_impl();

	resolver_impl(const resolver_impl &) = delete;

	/**
	 * Resolve a query string into a list of matching stream_info's on the network.
	 *
	 * Blocks until at least the minimum number of streams has been resolved, or the timeout fires,
	 * or the resolve has been cancelled.
	 * @param query The query string to send (usually a set of conditions on the properties of the
	 * stream info that should be searched, for example "name='BioSemi' and type='EEG'" (without the
	 * outer ""). See lsl_stream_info_matches_query() for the definition of a query.
	 * @param minimum The minimum number of unique streams that should be resolved before this
	 * function may to return.
	 * @param timeout The timeout after which this function is forced to return (even if it did not
	 * produce the desired number of results).
	 * @param minimum_time Search for matching streams for at least this much time (e.g., if
	 * multiple streams may be present).
	 */
	std::vector<stream_info_impl> resolve_oneshot(const std::string &query, int minimum = 0,
		double timeout = FOREVER, double minimum_time = 0.0);

	/**
	 * Starts a background thread that resolves a query string and periodically updates the list of
	 * present streams.
	 *
	 * After this, the resolver can *not* be repurposed for other queries or for
	 * oneshot operation (a new instance needs to be created for that).
	 * @param query The query string to send (usually a set of conditions on the properties of the
	 * stream info that should be searched, for example `name='BioSemi' and type='EEG'`
	 * See stream_info_impl::matches_query() for the definition of a query.
	 * @param forget_after If a stream vanishes from the network (e.g., because it was shut down),
	 * it will be pruned from the list this many seconds after it was last seen.
	 */
	void resolve_continuous(const std::string &query, double forget_after = 5.0);

	/// Get the current set of results (e.g., during continuous operation).
	std::vector<stream_info_impl> results(uint32_t max_results = 4294967295);

	/**
	 * Tear down any ongoing operations and render the resolver unusable.
	 *
	 * This can be used to cancel a blocking resolve_oneshot() from another thread (e.g., to
	 * initiate teardown of the object).
	 */
	void cancel();

private:
	/// This function starts a new wave of resolves.
	void next_resolve_wave();
	std::shared_ptr<class resolve_attempt> create_attempt(const std::string& query);

	// constants (mostly config-deduced)
	/// the list of multicast endpoints under consideration
	endpoint_list mcast_endpoints_;
	/// the list of per-host UDP endpoints under consideration
	endpoint_list ucast_endpoints_;

	// things related to cancellation
	/// if set, no more resolves can be started (destructively cancelled).
	std::atomic<bool> cancelled_{false};

	// reinitialized for each query
	/// forget results that are older than this (continuous operation only)
	double forget_after_{FOREVER};

	/// a shared pointer to the current resolve operation
	std::shared_ptr<class resolve_attempt> current_resolve;

	/// a thread that runs background IO if we are performing a resolve_continuous
	std::shared_ptr<std::thread> background_io_;
};

} // namespace lsl

#endif

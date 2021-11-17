#include "tcp_server.h"
#include "api_config.h"
#include "consumer_queue.h"
#include "sample.h"
#include "send_buffer.h"
#include "socket_utils.h"
#include "stream_info_impl.h"
#include "util/cast.hpp"
#include "util/endian.hpp"
#include "util/strfuns.hpp"
#include <asio/io_context.hpp>
#include <asio/ip/host_name.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <istream>
#include <loguru.hpp>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#ifdef _MSC_VER
// (inefficiently converting int to bool in portable_oarchive instantiation...)
#pragma warning(disable : 4800)
#endif

// a convention that applies when including portable_oarchive.h in multiple .cpp files.
// otherwise, the templates are instantiated in this file and sample.cpp which leads
// to errors like "multiple definition of `typeinfo name"
#define NO_EXPLICIT_TEMPLATE_INSTANTIATION
#include "portable_archive/portable_oarchive.hpp"

using std::size_t;

namespace lsl {
/**
 * Active session with a TCP client.
 *
 * A note on memory ownership:
 * - Generally, the stream_outlet maintains shared ownership of the `tcp_server`, `io_context`s,
 * and `stream_info`.
 * - At any point in time there are likely multiple request/handler chains in flight somewhere
 * between the operating system, asio, and the various handlers below.
 * The handlers are set up such that any memory that may be referred to by them in the future is
 * owned (shared) by the handler/callback function objects (this is what is encapsulated by the
 * client_session instance).
 * Their lifetime is managed by asio and ends when the handler chain ends (e.g., is aborted).
 * Since the TCP server is referred to (occasionally) by handler code, the `client_session`s store
 * a std::weak_ptr to the tcp_server that's upgraded to a std::shared_ptr as needed
 * - There is a per-session transfer thread (client_session::transfer_samples_thread()) that owns
 * the respective `client_session` which goes out of scope once the server is being shut down.
 * - The TCP server and client session also have shared ownership of the io_context (since in
 * some cases some transfer threads can outlive the stream outlet, and so the io_context is still
 * kept around until all sockets have been properly released).
 * - So memory is generally owned by the code (functors and stack frames) that needs to refer to
 * it for the duration of the execution.
 */
class client_session : public std::enable_shared_from_this<client_session> {

public:
	/// Instantiate a new session & its socket.
	client_session(const tcp_server_p &serv, tcp_socket &&sock)
		: io_(serv->io_), serv_(serv), sock_(std::move(sock)), requeststream_(&requestbuf_) {
		LOG_F(1, "Initialized client session %p", this);
	}

	/// Destructor.
	~client_session();

	/// Get the socket of this session.
	tcp_socket &socket() { return sock_; }

	/// Begin processing this session (i.e., data transmission over the socket).
	void begin_processing();

private:
	/// Handler that gets called when the reading of the 1st line (command line) of the inbound
	/// message finished.
	void handle_read_command_outcome(err_t err);

	/// Handler that gets called after finishing reading of the query line.
	void handle_read_query_outcome(err_t err);

	/// Helper function to send a status message to the connected party.
	void send_status_message(const std::string &msg);

	/// Handler that gets called after finishing the reading of feedparameters.
	void handle_read_feedparams(int request_protocol_version, std::string request_uid, err_t err);

	/// Handler that gets called sending the feedheader has completed.
	void handle_send_feedheader_outcome(err_t err, std::size_t n);

	/// Transfers samples from the server's send buffer into the async send queues of IO threads
	void transfer_samples_thread(std::shared_ptr<client_session> /*keepalive*/,
		std::shared_ptr<consumer_queue> &&queue, int max_samples_per_chunk);

	/// Handler that gets called when a sample transfer has been completed.
	void handle_chunk_transfer_outcome(err_t err, std::size_t len);

	/// shared pointer to IO service; ensures that the IO is still around by the time the serv_ and
	/// sock_ need to be destroyed
	io_context_p io_;
	/// the server that is associated with this connection
	std::weak_ptr<tcp_server> serv_;
	/// connection socket
	tcp_socket sock_;

	// data used by the transfer thread (and some other handlers)
	/// this buffer holds the data feed generated by us
	asio::streambuf feedbuf_;
	/// this buffer holds the request as received from the client (incrementally filled)
	asio::streambuf requestbuf_;
	/// output archive (wrapped around the feed buffer)
	std::unique_ptr<class eos::portable_oarchive> outarch_;
	/// this is a stream on top of the request buffer for convenient parsing
	std::istream requeststream_;
	/// scratchpad memory (e.g., for endianness conversion)
	char *scratch_{nullptr};
	/// protocol version to use for transmission
	int data_protocol_version_{100};
	/// is the client's endianness reversed (big<->little endian)
	bool reverse_byte_order_{false};
	/// our chunk granularity
	int chunk_granularity_{0};
	/// maximum number of samples buffered
	int max_buffered_{0};

	// data exchanged between the transfer completion handler and the transfer thread
	/// whether the current transfer has finished (possibly with an error)
	bool transfer_completed_;
	/// the outcome of the last chunk transfer
	asio::error_code transfer_error_;
	/// the amount of bytes transferred
	std::size_t transfer_amount_;
	/// a mutex that protects the completion data
	std::mutex completion_mut_;
	/// a condition variable that signals completion
	std::condition_variable completion_cond_;
};

class sync_transfer_handler {
	bool transfer_is_sync_;
	// sockets that should receive data in sync mode
	std::vector<tcp_socket_p> sync_sockets_;
	// io context for sync mode, app is responsible for running it
	asio::io_context io_ctx_;
public:
	sync_transfer_handler(): io_ctx_(1) {

	}

	/// schedules a native socket handle to be added the next time a push operation is done
	void add_socket(const tcp_socket::native_handle_type handle, tcp_socket::protocol_type protocol) {
		asio::post(io_ctx_, [=](){
			sync_sockets_.push_back(std::make_unique<tcp_socket>(io_ctx_, protocol, handle));
		});
	}
	void write_all_blocking(const std::vector<asio::const_buffer> &bufs);
};

tcp_server::tcp_server(stream_info_impl_p info, io_context_p io, send_buffer_p sendbuf,
	factory_p factory, int chunk_size, bool allow_v4, bool allow_v6, bool do_sync)
	: chunk_size_(chunk_size), info_(std::move(info)), io_(std::move(io)),
	  factory_(std::move(factory)), send_buffer_(std::move(sendbuf)) {
	if (do_sync) sync_handler = std::make_unique<sync_transfer_handler>();
	// assign connection-dependent fields
	info_->session_id(api_config::get_instance()->session_id());
	info_->reset_uid();
	info_->created_at(lsl_clock());
	info_->hostname(asio::ip::host_name());

	if (allow_v4) {
		try {
			acceptor_v4_ = std::make_unique<tcp_acceptor>(*io_, asio::ip::tcp::v4());
			auto port = bind_and_listen_to_port_in_range(*acceptor_v4_, asio::ip::tcp::v4(), 10);
			info_->v4data_port(port);
			LOG_F(1, "Created IPv%d TCP acceptor for %s @ port %d", 4, info_->name().c_str(), port);
		} catch (std::exception &e) {
			LOG_F(WARNING, "Failed to create IPv%d acceptor: %s", 4, e.what());
			acceptor_v4_.reset();
		}
	}
	if (allow_v6) {
		try {
			acceptor_v6_ = std::make_unique<tcp_acceptor>(*io_, asio::ip::tcp::v6());
			auto port = bind_and_listen_to_port_in_range(*acceptor_v6_, asio::ip::tcp::v6(), 10);
			info_->v6data_port(port);
			LOG_F(1, "Created IPv%d TCP acceptor for %s @ port %d", 6, info_->name().c_str(), port);
		} catch (std::exception &e) {
			LOG_F(WARNING, "Failed to create IPv%d acceptor: %s", 6, e.what());
			acceptor_v6_.reset();
		}
	}
	if (!acceptor_v4_ && !acceptor_v6_)
		throw std::runtime_error("Failed to instantiate socket acceptors for the TCP server");
}

tcp_server::~tcp_server() noexcept
{
	// defined here so the compiler can generate the destructor for the sync_handler
}


// === externally issued asynchronous commands ===

void tcp_server::begin_serving() {
	// pre-generate the info's messages
	shortinfo_msg_ = info_->to_shortinfo_message();
	fullinfo_msg_ = info_->to_fullinfo_message();
	// start accepting connections
	if (acceptor_v4_) accept_next_connection(acceptor_v4_);
	if (acceptor_v6_) accept_next_connection(acceptor_v6_);
}

void tcp_server::end_serving() {
	// issue closure of the server socket; this will result in a cancellation of the associated IO
	// operations
	post(*io_, [this, shared_this = shared_from_this()]() {
		if (acceptor_v4_) acceptor_v4_->close();
		if (acceptor_v6_) acceptor_v6_->close();
	});
	// issue closure of all active client session sockets; cancels the related outstanding IO jobs
	close_inflight_sessions();
	// also notify any transfer threads that are blocked waiting for a sample by sending them one (=
	// a ping)
	send_buffer_->push_sample(factory_->new_sample(lsl_clock(), true));
}

// === accept loop ===


void tcp_server::accept_next_connection(tcp_acceptor_p &acceptor) {
	try {
		// accept a new connection
		acceptor->async_accept(*io_, [shared_this = shared_from_this(), &acceptor](
										 err_t err, tcp_socket sock) {
			if (err == asio::error::operation_aborted || err == asio::error::shut_down) return;

			// no error: create a new session and start processing
			if (!err)
				std::make_shared<client_session>(shared_this, std::move(sock))->begin_processing();
			else
				LOG_F(WARNING, "Unhandled accept error: %s", err.message().c_str());

			// and move on to the next connection
			shared_this->accept_next_connection(acceptor);
		});
	} catch (std::exception &e) {
		LOG_F(ERROR, "Error during tcp_server::accept_next_connection: %s", e.what());
	}
}


// === synchronous transfer

void tcp_server::write_all_blocking(const std::vector<asio::const_buffer> &bufs)
{
	sync_handler->write_all_blocking(bufs);
}

void sync_transfer_handler::write_all_blocking(const std::vector<asio::const_buffer> &bufs) {
	bool any_session_broken = false;

	for (auto &sock : sync_sockets_) {
		asio::async_write(*sock, bufs,
			[this, &sock, &any_session_broken](
				const asio::error_code &ec, size_t bytes_transferred) {
				switch (ec.value()) {
				case 0: break; // success
				case asio::error::broken_pipe:
				case asio::error::connection_reset:
					LOG_F(WARNING, "Broken Pipe / Connection Reset detected. Closing socket.");
					any_session_broken = true;
					asio::post(io_ctx_, [sock]() {
						asio::error_code close_ec;
						sock->close(close_ec);
					});
					break;
				case asio::error::operation_aborted:
					LOG_F(INFO, "Socket wasn't fast enough");
					break;
				default:
					LOG_F(ERROR, "Unhandled write_all_blocking error: %s.", ec.message().c_str());
				}
			});
	}
	try {
		// prepare the io context for new work
		io_ctx_.restart();
		io_ctx_.run();

		if (any_session_broken) {
			// remove sessions whose socket was closed
			auto new_end_it = std::remove_if(sync_sockets_.begin(), sync_sockets_.end(),
				[](const tcp_socket_p &sock) {
					return !sock->is_open();
				});
			sync_sockets_.erase(new_end_it, sync_sockets_.end());
		}
	} catch (std::exception &e) { LOG_F(ERROR, "Error during write_all_blocking: %s", e.what()); }
}

void tcp_server::register_inflight_session(const std::shared_ptr<client_session> &session) {
	std::lock_guard<std::recursive_mutex> lock(inflight_mut_);
	inflight_.insert(std::make_pair(session.get(), session));
}

void tcp_server::unregister_inflight_session(client_session *session) {
	std::lock_guard<std::recursive_mutex> lock(inflight_mut_);
	auto pos = inflight_.find(session);
	if (pos != inflight_.end()) inflight_.erase(pos);
}

void tcp_server::close_inflight_sessions() {
	std::lock_guard<std::recursive_mutex> lock(inflight_mut_);
	for (auto &pair : inflight_) {
		auto session = pair.second.lock();
		// session has already expired on its own
		if (!session) continue;
		post(session->socket().get_executor(), [session]() {
			asio::error_code ec;
			auto &sock = session->socket();
			if (sock.is_open()) {
				sock.shutdown(sock.shutdown_both, ec);
				sock.close(ec);
				if (ec) LOG_F(WARNING, "Error during shutdown_and_close: %s", ec.message().c_str());
			}
		});
	}
	inflight_.clear();
}

// === implementation of the client_session class ===

client_session::~client_session() {
	LOG_F(1, "Destructing session %p", this);
	delete[] scratch_;
	if (auto serv = serv_.lock()) serv->unregister_inflight_session(this);
}

void client_session::begin_processing() {
	try {
		sock_.set_option(asio::ip::tcp::no_delay(true));
		if (api_config::get_instance()->socket_send_buffer_size() > 0)
			sock_.set_option(asio::socket_base::send_buffer_size(
				api_config::get_instance()->socket_send_buffer_size()));
		if (api_config::get_instance()->socket_receive_buffer_size() > 0)
			sock_.set_option(asio::socket_base::receive_buffer_size(
				api_config::get_instance()->socket_receive_buffer_size()));
		// register this socket as "in-flight" with the server (so that any subsequent ops on it can
		// be aborted if necessary)
		if (auto serv = serv_.lock()) {
			serv->register_inflight_session(shared_from_this());
			// read the request line
			async_read_until(sock_, requestbuf_, "\r\n",
				[shared_this = shared_from_this()](err_t err, std::size_t /*unused*/) {
					shared_this->handle_read_command_outcome(err);
				});
		} else {
			throw std::runtime_error("server disappeared before start client session");
		}
	} catch (std::exception &e) {
		LOG_F(ERROR, "Error during client_session::begin_processing: %s", e.what());
	}
}

void client_session::handle_read_command_outcome(err_t read_err) {
	try {
		if (read_err) return;
		// parse request method
		std::string method;
		getline(requeststream_, method);
		method = trim(method);
		if (method == "LSL:shortinfo")
			// shortinfo request: read the content query string
			async_read_until(sock_, requestbuf_, "\r\n",
				[shared_this = shared_from_this()](err_t err, std::size_t /*unused*/) {
					shared_this->handle_read_query_outcome(err);
				});
		else if (method == "LSL:fullinfo") {
			// fullinfo request: reply right away
			auto serv = serv_.lock();
			if (serv)
				async_write(sock_, asio::buffer(serv->fullinfo_msg_),
					[shared_this = shared_from_this(), serv](
						err_t /*unused*/, std::size_t /*unused*/) {});
		} else if (method == "LSL:streamfeed")
			// streamfeed request (1.00): read feed parameters
			async_read_until(sock_, requestbuf_, "\r\n",
				[shared_this = shared_from_this()](err_t err, std::size_t /*unused*/) {
					shared_this->handle_read_feedparams(100, "", err);
				});
		else if (method.compare(0, 15, "LSL:streamfeed/") == 0) {
			// streamfeed request with version: read feed parameters
			std::vector<std::string> parts = splitandtrim(method, ' ', true);
			async_read_until(sock_, requestbuf_, "\r\n\r\n",
				[shared_this = shared_from_this(),
					request_protocol_version = std::stoi(parts[0].substr(15)),
					request_uid = (parts.size() > 1) ? parts[1] : ""](
					err_t err, std::size_t /*unused*/) {
					shared_this->handle_read_feedparams(request_protocol_version, request_uid, err);
				});
		}
	} catch (std::exception &e) {
		LOG_F(WARNING, "Unexpected error while parsing a client command: %s", e.what());
	}
}

void client_session::handle_read_query_outcome(err_t err) {
	try {
		if (err) return;
		// read the query line
		std::string query;
		getline(requeststream_, query);
		query = trim(query);
		auto serv = serv_.lock();
		if (!serv) return;
		if (serv->info_->matches_query(query)) {
			// matches: reply (otherwise just close the stream)
			async_write(sock_, asio::buffer(serv->shortinfo_msg_),
				[serv](err_t /*unused*/, std::size_t /*unused*/) {
					/* keep the tcp_server alive until the shortinfo is sent completely*/
				});
		} else {
			DLOG_F(INFO, "%p got a shortinfo query response for the wrong query", this);
		}
	} catch (std::exception &e) {
		LOG_F(WARNING, "Unexpected error while parsing a client request: %s", e.what());
	}
}

void client_session::send_status_message(const std::string &msg) {
	auto buf(std::make_shared<std::string>(msg));
	async_write(sock_, asio::buffer(*buf),
		[buf, shared_this = shared_from_this()](err_t /*unused*/,
			std::size_t /*unused*/) { /* keep objects alive until the message is sent */ });
}

void client_session::handle_read_feedparams(
	int request_protocol_version, std::string request_uid, err_t err) {
	try {
		if (err) return;
		DLOG_F(2, "%p got a streamfeed request", this);
		// --- protocol negotiation ---

		// check request validity
		if (request_protocol_version / 100 >
			api_config::get_instance()->use_protocol_version() / 100) {
			send_status_message("LSL/" +
								std::to_string(api_config::get_instance()->use_protocol_version()) +
								" 505 Version not supported");
			DLOG_F(WARNING, "%p Got a request for a too new protocol version", this);
			return;
		}
		auto serv = serv_.lock();
		if (!serv) return;
		auto &info = serv->info_;
		if (!request_uid.empty() && request_uid != info->uid()) {
			send_status_message("LSL/" +
								to_string(api_config::get_instance()->use_protocol_version()) +
								" 404 Not found");
			return;
		}

		if (request_protocol_version >= 110) {
			int client_byte_order = 1234;		  // assume little endian
			double client_endian_performance = 0; // the other party's endian conversion performance
			bool client_has_ieee754_floats =
				true; // the client has IEEE-754 compliant floating point formats
			bool client_supports_subnormals = true; // the client supports subnormal numbers
			int client_protocol_version =
				request_protocol_version; // assume that the client wants to use the same
										  // version for data transmission
			int client_value_size = info->channel_bytes(); // assume that the client has a standard
														   // size for the relevant data type
			lsl_channel_format_t format = info->channel_format();

			// read feed parameters
			char buf[16384] = {0};
			while (requeststream_.getline(buf, sizeof(buf)) && (buf[0] != '\r')) {
				std::string hdrline(buf);
				std::size_t colon = hdrline.find_first_of(':');
				if (colon != std::string::npos) {
					// strip off comments
					auto semicolon = hdrline.find_first_of(';');
					if (semicolon != std::string::npos) hdrline.erase(semicolon);
					// convert to lowercase
					for (auto &c : hdrline) c = ::tolower(c);
					// extract key & value
					std::string type = trim(hdrline.substr(0, colon)),
								rest = trim(hdrline.substr(colon + 1));
					// get the header information
					if (type == "native-byte-order") client_byte_order = std::stoi(rest);
					if (type == "endian-performance")
						client_endian_performance = std::stod(rest);
					if (type == "has-ieee754-floats")
						client_has_ieee754_floats = from_string<bool>(rest);
					if (type == "supports-subnormals")
						client_supports_subnormals = from_string<bool>(rest);
					if (type == "value-size") client_value_size = std::stoi(rest);
					if (type == "max-buffer-length") max_buffered_ = std::stoi(rest);
					if (type == "max-chunk-length") chunk_granularity_ = std::stoi(rest);
					if (type == "protocol-version") client_protocol_version = std::stoi(rest);
				} else {
					DLOG_F(WARNING, "%p Request line '%s' contained no key-value pair", this,
						hdrline.c_str());
				}
			}

			// determine the parameters for data transmission
			bool client_suppress_subnormals = false;

			lsl::Endianness use_byte_order = LSL_BYTE_ORDER;

			// use least common denominator data protocol version
			data_protocol_version_ = std::min(
				api_config::get_instance()->use_protocol_version(), client_protocol_version);
			// downgrade to 1.00 (portable binary format) if an unsupported binary conversion is
			// involved
			if (info->channel_format() != cft_string && info->channel_bytes() != client_value_size)
				data_protocol_version_ = 100;
			if (!format_ieee754[cft_double64] ||
				(format == cft_float32 && !format_ieee754[cft_float32]) ||
				!client_has_ieee754_floats)
				data_protocol_version_ = 100;
			if (data_protocol_version_ >= 110) {

				// enable endian conversion when
				// 1. our byte ordering is different from the client's *and*
				// 2. we can actually perform the conversion *and*
				// 3. the sample format is wide enough for endianness to matter *and*
				// 4. we're faster at converting than the client
				if (LSL_BYTE_ORDER != client_byte_order &&						  // (1)
					lsl::can_convert_endian(client_byte_order, client_value_size) // (2)
					&& client_value_size > 1 &&									  // (3)
					(measure_endian_performance() > client_endian_performance))	  // (4)
				{
					use_byte_order = static_cast<lsl::Endianness>(client_byte_order);
					reverse_byte_order_ = true;
				}

				// determine if subnormal suppression needs to be enabled
				client_suppress_subnormals =
					(format_subnormal[format] && !client_supports_subnormals);
			}

			// send the response
			std::ostream response_stream(&feedbuf_);
			response_stream << "LSL/" << api_config::get_instance()->use_protocol_version()
							<< " 200 OK\r\n";
			response_stream << "UID: " << info->uid() << "\r\n";
			response_stream << "Byte-Order: " << use_byte_order << "\r\n";
			response_stream << "Suppress-Subnormals: " << client_suppress_subnormals << "\r\n";
			response_stream << "Data-Protocol-Version: " << data_protocol_version_ << "\r\n";
			response_stream << "\r\n" << std::flush;
		} else {
			// read feed parameters
			requeststream_ >> max_buffered_ >> chunk_granularity_;
		}

		// --- validation ---
		if (data_protocol_version_ == 100) {
			// create a portable output archive to write to
			outarch_ = std::make_unique<eos::portable_oarchive>(feedbuf_);
			// serialize the shortinfo message into an archive
			*outarch_ << serv->shortinfo_msg_;
		} else {
			// allocate scratchpad memory for endian conversion, etc.
			scratch_ = new char[format_sizes[info->channel_format()] * info->channel_count()];
		}

		// send test pattern samples
		lsl::factory fac(info->channel_format(), info->channel_count(), 4);

		for (int test_pattern : {4, 2}) {
			lsl::sample_p temp(fac.new_sample(0.0, false));
			temp->assign_test_pattern(test_pattern);
			if (data_protocol_version_ >= 110)
				temp->save_streambuf(
					feedbuf_, data_protocol_version_, reverse_byte_order_, scratch_);
			else
				*outarch_ << *temp;
		}

		// send off the newly created feedheader
		async_write(
			sock_, feedbuf_.data(), [shared_this = shared_from_this()](err_t err, std::size_t len) {
				shared_this->handle_send_feedheader_outcome(err, len);
			});
		DLOG_F(2, "%p sent test pattern samples", this);
	} catch (std::exception &e) {
		LOG_F(WARNING, "Unexpected error while serializing the feed header: %s", e.what());
	}
}

void client_session::handle_send_feedheader_outcome(err_t err, std::size_t n) {
	try {
		if (err) return;

		feedbuf_.consume(n);

		auto serv = serv_.lock();
		if (!serv) return;

		// quit if max_buffered_ is 0. This is a bit unexpected, but backwards compatible and quite
		// convenient for unit tests
		if (max_buffered_ <= 0) return;

		if (serv->sync_handler) {
			LOG_F(INFO, "Using synchronous blocking transfers for new client session.");
			auto protocol = sock_.local_endpoint().protocol();
			// move the socket into the sync_transfer_io_ctx by releasing it from this
			// io ctx and re-creating it with sync_transfer_io_ctx.
			// See https://stackoverflow.com/q/52671836/73299
			// Then schedule the sync_transfer_io_ctx to add it to the list of sync sockets
			serv->sync_handler->add_socket(sock_.release(), protocol);
			serv->unregister_inflight_session(this);
			return;
		}

		// determine transfer parameters
		auto queue = serv->send_buffer_->new_consumer(max_buffered_);

		// determine the maximum chunk size
		int max_samples_per_chunk = std::numeric_limits<int>::max();
		if (chunk_granularity_)
			max_samples_per_chunk = chunk_granularity_;
		else if (serv->chunk_size_)
			max_samples_per_chunk = serv->chunk_size_;

		// spawn a sample transfer thread.
		std::thread(&client_session::transfer_samples_thread, this, shared_from_this(),
			std::move(queue), max_samples_per_chunk)
			.detach();
	} catch (std::exception &e) {
		LOG_F(WARNING, "Unexpected error while handling the feedheader send outcome: %s", e.what());
	}
}

void client_session::transfer_samples_thread(std::shared_ptr<client_session> /* keepalive */,
	std::shared_ptr<consumer_queue> &&queue, int max_samples_per_chunk) {
	int samples_in_current_chunk = 0;
	while (!serv_.expired()) {
		try {
			// get next sample from the sample queue (blocking)
			sample_p samp(queue->pop_sample());

			// ignore blank samples (they are basically wakeup notifiers from someone's
			// end_serving())
			if (!samp) continue;
			// serialize the sample into the stream
			if (data_protocol_version_ >= 110)
				samp->save_streambuf(
					feedbuf_, data_protocol_version_, reverse_byte_order_, scratch_);
			else
				*outarch_ << *samp;
			// if the sample is marked as force-push or the configured chunk size is reached
			if (samp->pushthrough || ++samples_in_current_chunk >= max_samples_per_chunk) {
				// send off the chunk that we aggregated so far
				std::unique_lock<std::mutex> lock(completion_mut_);
				transfer_completed_ = false;
				async_write(sock_, feedbuf_.data(),
					[shared_this = shared_from_this()](err_t err, std::size_t len) {
						shared_this->handle_chunk_transfer_outcome(err, len);
					});
				// wait for the completion condition
				completion_cond_.wait(lock, [this]() { return transfer_completed_; });
				// handle transfer outcome
				if (!transfer_error_) {
					feedbuf_.consume(transfer_amount_);
				} else
					break;
				samples_in_current_chunk = 0;
			}
		} catch (std::exception &e) {
			LOG_F(WARNING, "Unexpected glitch in transfer_samples_thread: %s", e.what());
		}
	}
}

void client_session::handle_chunk_transfer_outcome(err_t err, std::size_t len) {
	try {
		{
			std::lock_guard<std::mutex> lock(completion_mut_);
			// assign the transfer outcome
			transfer_error_ = err;
			transfer_amount_ = len;
			transfer_completed_ = true;
		}
		// notify the server thread
		completion_cond_.notify_all();
	} catch (std::exception &e) {
		LOG_F(WARNING,
			"Catastrophic error in handling the chunk transfer outcome (in tcp_server): %s",
			e.what());
	}
}
} // namespace lsl

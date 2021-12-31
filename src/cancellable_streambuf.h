#pragma once
//
// cancellable_streambuf.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// This is a carbon copy of basic_socket_streambuf.hpp, adding a cancel() member function
// (an removing support for unbuffered I/O).
//
// Copyright (c) 2003-2011 Christopher M. Kohlhoff (chris at kohlhoff dot com)
// Modified by Christian A. Kothe, 2012
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#define BOOST_ASIO_NO_DEPRECATED
#include "cancellation.h"
#include <array>
#include <asio/basic_stream_socket.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <exception>
#include <streambuf>

using asio::io_context;

namespace lsl {
using Protocol = asio::ip::tcp;
using Socket = asio::basic_stream_socket<Protocol, asio::io_context::executor_type>;
/// Iostream streambuf for a socket.
class cancellable_streambuf final : public std::streambuf,
									private asio::io_context,
									private Socket,
									public lsl::cancellable_obj {
public:
	/// Construct a cancellable_streambuf without establishing a connection.
	cancellable_streambuf() : io_context(1), Socket(as_context()) { init_buffers(); }

	/// Destructor flushes buffered data.
	~cancellable_streambuf() override {
		// no cancel() can fire after this call
		unregister_from_all();
		if (pptr() != pbase()) overflow(traits_type::eof());
	}

	/**
	 * Cancel the current stream operations destructively.
	 * All blocking operations will fail after a cancel() has been issued,
	 * and the stream buffer cannot be reused.
	 */
	void cancel() override;


	/// Establish a connection.
	/**
	 * This function establishes a connection to the specified endpoint.
	 *
	 * @return \c this if a connection was successfully established, a null
	 * pointer otherwise.
	 */
	cancellable_streambuf *connect(const Protocol::endpoint &endpoint) {
		if (cancelled_)
			throw std::logic_error("Attempt to connect() a cancelled streambuf");

		init_buffers();
		socket().close(ec_);
		socket().async_connect(endpoint, [this](const asio::error_code &ec) { this->ec_ = ec; });
		run_io_ctx();
		return !ec_ ? this : nullptr;
	}

	/// Close the connection.
	/**
	 * @return \c this if a connection was successfully established, a null
	 * pointer otherwise.
	 */
	cancellable_streambuf *close() {
		sync();
		close_if_open();
		return !ec_ ? this : nullptr;
	}

	/** Get the last error associated with the stream buffer.
	 * @return An \c error_code corresponding to the last error from the stream
	 * buffer.
	 */
	const asio::error_code &error() const { return ec_; }

protected:
	/// Close the socket if it's open.
	void close_if_open() {
		cancelled_ = true;
		socket().shutdown(Socket::shutdown_both, ec_);
		socket().close(ec_);
	}

	/// Estimate bytes available in the buffer and the socket's OS buffer
	std::streamsize showmanyc() override {
		asio::error_code ec;
		return egptr() - gptr() + static_cast<std::streamsize>(socket().available(ec));
	}

	/// Convenience method to call methods inherited from `Socket`
	Socket &socket() { return *this; }

	/// Convenience method to call methods inherited from `io_context`
	asio::io_context &as_context() { return *this; }

	/// read bytes from the socket into the get buffer
	int_type underflow() override;

	/// optimized version of xsgetn, skips the input buffer on underflow
	std::streamsize xsgetn( char_type* s, std::streamsize count ) override;

	/// flush the output buffer to the socket
	int_type overflow(int_type c) override;

	/// flush the output buffer to the socket
	int sync() override { return overflow(traits_type::eof()); }

	std::streambuf *setbuf(char_type * /*unused*/, std::streamsize /*unused*/) override {
		// this feature was stripped out...
		return nullptr;
	}

private:
	/// Start an async receive operation that runs until enough data has been received
	void async_receive_all(std::array<asio::mutable_buffer, 2>& buf, std::size_t &bytes_transferred);

	/// run the io_context until it runs out of work, the streambuf is cancelled or an error occurs
	bool run_io_ctx() noexcept;

	/// receive data into an (optional) user supplied buffer and the `get_buffer_`
	std::streamsize recv(char* target = nullptr, std::size_t target_size = 0);

	/** Resets the get buffer positions
	 * @param bytes_available Number of already retrieved bytes at the beginning of the buffer
	 */
	void init_get_buffer(std::size_t bytes_available = 0) {
		setg(&get_buffer_[0], &get_buffer_[0] + putback_max,
			&get_buffer_[0] + putback_max + bytes_available);
	}

	void init_buffers() {
		init_get_buffer();
		setp(&put_buffer_[0], &put_buffer_[0] + sizeof(put_buffer_));
	}

	enum { putback_max = 8, buffer_size = 16384 };
	std::atomic<bool> cancelled_{false};
	char get_buffer_[buffer_size]{0}, put_buffer_[buffer_size]{0};
	asio::error_code ec_;
	std::mutex mutex_io_ctx_;
};
} // namespace lsl

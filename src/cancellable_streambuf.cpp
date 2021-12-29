#include "cancellable_streambuf.h"
#include "loguru.hpp"
#include <asio/write.hpp>
#include <thread>

void lsl::cancellable_streambuf::async_receive_all(
	std::array<asio::mutable_buffer, 2> &bufs, std::size_t &bytes_transferred) {
	socket().async_receive(
		bufs, [this, &bufs, &bytes_transferred](const asio::error_code &ec, std::size_t n) {
			bytes_transferred += n;
			// not yet read enough? schedule another async read
			if (!ec && n < bufs[0].size()) {
				bufs[0] += n;
				async_receive_all(bufs, bytes_transferred);
			}
			this->ec_ = ec;
	});
}

bool lsl::cancellable_streambuf::try_wait_for_runnable_state() noexcept
{
	// https://rigtorp.se/spinlock/
	while(true) {
		// Optimistically assume the lock is free on the first try
		if(!running_.exchange(true, std::memory_order_acquire)) return true;

		// Wait for lock to be released without generating cache misses
		while(running_.load(std::memory_order_relaxed)) {
			if(cancelled_.load(std::memory_order_relaxed)) return false;
			else std::this_thread::yield();
		}
	}
}

bool lsl::cancellable_streambuf::run_io_ctx() noexcept
{
	if(!try_wait_for_runnable_state()) return false;

	ec_ = asio::error_code();
	try {
		as_context().restart();
		// run until we've been cancelled, run out of work or encountered an error
		while(true)
			if(cancelled_ || !as_context().run_one() || ec_) break;
	} catch(std::exception& e) {
		LOG_F(ERROR, "Error in cancellable_streambuf::run_io_ctx: %s", e.what());
		running_.store(false, std::memory_order_release);
		throw;
	}
	// release the lock
	running_.store(false, std::memory_order_release);
	return !cancelled_ && !ec_;
}

std::streamsize lsl::cancellable_streambuf::recv(char *target, std::size_t target_size) {
	std::size_t bytes_transferred = 0;
	std::array<asio::mutable_buffer, 2> bufs = {asio::buffer(target, target_size),
		asio::buffer(&get_buffer_[0] + putback_max, buffer_size - putback_max)};

	if (target_size)
		async_receive_all(bufs, bytes_transferred);
	else
		socket().async_receive(
			bufs[1], [this, &bytes_transferred](const asio::error_code &ec, std::size_t n) {
				bytes_transferred = n;
				this->ec_ = ec;
			});

	if (!run_io_ctx()) return traits_type::eof();

	init_get_buffer(bytes_transferred - target_size);
	return static_cast<std::streamsize>(bytes_transferred);
}

void lsl::cancellable_streambuf::cancel()
{
	if(!running_.exchange(true)) {
		// no io_ctx running: close the socket in this thread
		close_if_open();
	} else {
		asio::post(as_context(), [this](){ close_if_open(); });
		// wait for a potentially running io_ctx to finish
		try_wait_for_runnable_state();

		// double check that the socket is really closed, i.e. the asio::post didn't happen after
		// the io_ctx finished and the spinlock running_ was reset
		if(socket().is_open())
			close_if_open();
	}
	running_ = false;
}

lsl::cancellable_streambuf::int_type lsl::cancellable_streambuf::underflow() {
	if (gptr() == egptr() && recv() > 0) return traits_type::to_int_type(*gptr());

	return traits_type::eof();
}

std::streamsize lsl::cancellable_streambuf::xsgetn(char_type *s, std::streamsize count) {
	auto buffered = std::min(egptr() - gptr(), count);

	// copy already buffered bytes first
	if (buffered > 0) {
		std::memcpy(s, gptr(), static_cast<std::size_t>(buffered));
		gbump(static_cast<int>(buffered));
	}
	// return if all requested bytes already copied from buffer
	if (buffered == count) return count;

	// receive remaining data, appending to `s` first and putting the remainder in the get buffer
	auto result = recv(s + buffered, static_cast<std::size_t>(count - buffered));
	return (result == traits_type::eof()) ? traits_type::eof() : count;
}

lsl::cancellable_streambuf::int_type lsl::cancellable_streambuf::overflow(int_type c) {
	// Send all data in the output buffer.
	asio::async_write(socket(), asio::buffer(pbase(), static_cast<std::size_t>(pptr() - pbase())),
		[this](const asio::error_code &ec, std::size_t /* unused */) { this->ec_ = ec; });

	run_io_ctx();
	if (ec_) return traits_type::eof();

	setp(put_buffer_, put_buffer_ + buffer_size);

	// If the new character is eof then our work here is done.
	if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);

	// Add the new character to the output buffer.
	*pptr() = traits_type::to_char_type(c);
	pbump(1);
	return c;
}

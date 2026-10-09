#pragma once

// AsyncHttpsGet: one HTTPS GET driven by a caller-supplied io_context.
//
// Why this exists alongside the blocking HttpsGet in rest.h: the warm-up has
// to overlap with the live WebSocket stream. A bar that closes while we are
// still fetching history is pushed exactly once, and if we are not subscribed
// yet it is gone - the symbol then runs one bar stale until its NEXT bar is
// classified as a gap, which can be an hour later. Subscribing first and
// buffering only works if the read loop keeps running while the REST calls
// are in flight, and on a single thread that means the REST calls cannot
// block.
//
// Each call owns its own connection: resolve, TLS handshake, write, read,
// shut down, then invoke the handler exactly once. There is no connection
// reuse - ~800 handshakes at startup is measurable, and whether it is worth a
// keep-alive pool is a Phase 1 question with a benchmark attached, not an
// assumption to bake in now.
//
// The handler is invoked exactly once on every path, with std::nullopt on any
// failure (DNS, TLS, timeout, non-200 status, malformed response). It never
// throws out of the io_context.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <optional>
#include <string>

namespace screener {

// std::nullopt on any failure; the body otherwise. Called on the thread
// running the io_context, from inside a completion handler.
using HttpResponseHandler = std::function<void(std::optional<std::string>)>;

// Starts the request and returns immediately. The session keeps itself alive
// through a shared_ptr until the handler has run, so the caller does not need
// to hold anything.
//
// `ssl_ctx` must outlive the request. Sharing one context with the WebSocket
// session is intended: a context is a certificate store plus options, and on
// one thread there is nothing to race.
void AsyncHttpsGet(boost::asio::io_context& ioc, boost::asio::ssl::context& ssl_ctx, std::string host, std::string port,
                   std::string target, HttpResponseHandler handler);

}  // namespace screener

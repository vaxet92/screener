#include "ws.h"

static void fail(beast::error_code ec, const char* what) {
    Logger::Log(LogLevel::kError, "[WebSocket] {}: {}", what, ec.message());
}

WebSocketSessionSSL::WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                         std::function<void(std::string_view)> onMessageCallback)
    : resolver(net::make_strand(ioc)),
      ws(net::make_strand(ioc), ctx),
      onMessageCallback(std::move(onMessageCallback)),
      stopped(false) {}

void WebSocketSessionSSL::Run(const char* inHost, const char* inPort, const char* inTarget) {
    if (stopped) {
        return;
    }

    host = inHost;
    target = inTarget;

    resolver.async_resolve(inHost, inPort,
                           beast::bind_front_handler(&WebSocketSessionSSL::OnResolve, shared_from_this()));
}

void WebSocketSessionSSL::Send(std::string text) {
    if (stopped) {
        return;
    }
    write_queue_.push_back(std::move(text));
    DrainWriteQueue();
}

void WebSocketSessionSSL::DrainWriteQueue() {
    // Three guards, each for its own reason: nothing to send; a write already
    // in flight (Beast permits exactly one); or the socket is not a WebSocket
    // yet, in which case the queue is drained from OnHandshake instead.
    if (write_queue_.empty() || writing_ || !open_ || stopped) {
        return;
    }
    writing_ = true;
    ws.async_write(net::buffer(write_queue_.front()),
                   beast::bind_front_handler(&WebSocketSessionSSL::OnWrite, shared_from_this()));
}

void WebSocketSessionSSL::OnWrite(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    writing_ = false;

    if (ec) {
        fail(ec, "write");
        return NotifyClosed();
    }
    if (stopped) {
        return;
    }

    // Pop only AFTER the write completed: async_write borrowed this string's
    // storage, so releasing it earlier would hand Beast a dangling buffer.
    if (!write_queue_.empty()) {
        write_queue_.pop_front();
    }
    DrainWriteQueue();
}

void WebSocketSessionSSL::NotifyClosed() {
    // Deliberate shutdown, or already reported - either way the owner must
    // not be asked to reconnect. See the header for why both guards matter.
    if (stopped || closed_notified_) {
        return;
    }
    open_ = false;
    closed_notified_ = true;
    if (on_closed_) {
        on_closed_();
    }
}

void WebSocketSessionSSL::OnResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        fail(ec, "resolve");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    beast::get_lowest_layer(ws).async_connect(
        results, beast::bind_front_handler(&WebSocketSessionSSL::OnConnect, shared_from_this()));
}

void WebSocketSessionSSL::OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type /*ep*/) {
    if (ec) {
        fail(ec, "connect");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    // SNI. Bybit terminates TLS behind a shared front end, so without the
    // server name the handshake gets the wrong certificate and fails.
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        fail(ec, "SSL SNI");
        return NotifyClosed();
    }

    ws.next_layer().async_handshake(
        ssl::stream_base::client, beast::bind_front_handler(&WebSocketSessionSSL::OnSslHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnSslHandshake(beast::error_code ec) {
    if (ec) {
        fail(ec, "ssl_handshake");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    beast::get_lowest_layer(ws).expires_never();

    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " screener");
    }));

    ws.async_handshake(host, target, beast::bind_front_handler(&WebSocketSessionSSL::OnHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnHandshake(beast::error_code ec) {
    if (ec) {
        fail(ec, "handshake");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }
    Logger::Log(LogLevel::kInfo, "[WebSocket] Handshake successful with {}{}", host, target);

    open_ = true;

    // Read loop FIRST, then the owner's frames. Bybit answers a subscribe
    // with an ack, and the ping timer the owner arms here expects a pong - if
    // the read loop were started after those writes, both replies could
    // already be in the socket buffer with nobody reading.
    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));

    if (on_open_) {
        on_open_();
    }
    DrainWriteQueue();
}

void WebSocketSessionSSL::OnRead(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        Logger::Log(LogLevel::kInfo, "[WebSocket] Connection closed by {}{}", host, target);
        return NotifyClosed();
    } else if (ec) {
        fail(ec, "read");
        return NotifyClosed();
    } else if (stopped) {
        return;
    }

    if (bytes_transferred > 0 && onMessageCallback) {
        // A view straight into the flat_buffer's storage - no copy. Valid
        // only until buffer.consume() below, so the callback (which runs the
        // entire synchronous parse chain before returning) MUST run first.
        const net::const_buffer data = buffer.data();
        onMessageCallback(std::string_view(static_cast<const char*>(data.data()), bytes_transferred));
    }
    buffer.consume(bytes_transferred);

    ws.async_read(buffer, beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::Stop() {
    stopped = true;
    open_ = false;
}

void WebSocketSessionSSL::OnClose(beast::error_code ec) {
    if (ec) {
        return fail(ec, "close");
    }
    Logger::Log(LogLevel::kInfo, "[WebSocket] Closed successfully: {}{}", host, target);
}

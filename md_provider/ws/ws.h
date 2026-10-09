#pragma once

// WebSocketSessionSSL: one Boost.Beast WSS connection - connect, TLS/WS
// handshake, a serialised write queue, the read loop, and a once-only,
// not-after-Stop() SetOnClosed callback the owner uses to drive reconnection.

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "logger/logger.h"
#include "root_certificates.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

#define WS_TIMER_RATE 30

class WebSocketSessionSSL : public std::enable_shared_from_this<WebSocketSessionSSL> {
   public:
    explicit WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                 std::function<void(std::string_view)> onMessageCallback);
    ~WebSocketSessionSSL() = default;

    void Run(const char* inHost, const char* inPort, const char* inTarget);
    void Stop();

    // Queue one text frame.
    //
    // A QUEUE and not a bare async_write: Beast allows exactly one write in
    // flight per stream, and this connection has two independent writers -
    // the batched subscribe frames at startup and the 20s ping timer forever
    // after. Without serialisation a ping landing between two subscribe
    // batches would be an overlapping write, which is undefined behaviour in
    // Beast rather than a queued send.
    //
    // Safe to call before the handshake completes: frames queued early are
    // sent in order once the socket is open.
    void Send(std::string text);

    // Fired once, after the WebSocket handshake succeeds. This is where the
    // owner sends its subscribe frames and arms its ping timer - doing either
    // before the handshake would write into a socket that is not a WebSocket
    // yet.
    void SetOnOpen(std::function<void()> callback) { on_open_ = std::move(callback); }

    // Called once when this session terminates for any reason the owner did
    // not ask for: connect/handshake failure, read error, or the venue
    // closing on us. NOT called after Stop() - a deliberate shutdown is not
    // something to reconnect from.
    //
    // Fires on the io_context thread, from inside a completion handler. The
    // owner must not block in it, and must not destroy this session from it -
    // post the reconnect instead.
    void SetOnClosed(std::function<void()> callback) { on_closed_ = std::move(callback); }

    bool IsOpen() const { return open_; }

   private:
    // Fires on_closed_ at most once, and never after Stop().
    //
    // once-only matters because the far end of this callback opens a
    // replacement socket. A session that notified twice would leave the owner
    // holding two sockets for one dead connection, and the count would drift
    // upward on every flap - eventually past the venue's connection limit.
    //
    // silent after Stop() for the mirror reason. During shutdown every
    // session dies; if each asked to be reconnected the process would never
    // exit.
    void NotifyClosed();

    void OnResolve(beast::error_code ec, tcp::resolver::results_type results);
    void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void OnSslHandshake(beast::error_code ec);
    void OnHandshake(beast::error_code ec);
    void OnWrite(beast::error_code ec, std::size_t bytes_transferred);
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void OnClose(beast::error_code ec);

    // Starts the next queued write if the socket is open and nothing is in
    // flight. The one place that may call async_write.
    void DrainWriteQueue();

    tcp::resolver resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws;
    beast::flat_buffer buffer;
    std::string host;
    std::string target;
    std::function<void(std::string_view)> onMessageCallback;
    std::function<void()> on_open_;
    std::function<void()> on_closed_;

    // Frames own their storage until the write completes: async_write only
    // borrows the buffer, so a string_view or a caller-owned buffer would
    // dangle the moment the caller returned.
    std::deque<std::string> write_queue_;
    bool writing_ = false;
    bool open_ = false;
    bool closed_notified_ = false;
    bool stopped;
};

using WebSocketSessionSSLPtr = std::shared_ptr<WebSocketSessionSSL>;

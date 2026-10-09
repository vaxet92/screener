#include "md_provider/async_rest.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

#include "logger/logger.h"

namespace screener {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

// Per-operation deadline on the stream.
//
// Not optional: the warm-up only completes when every request has called its
// handler, so one connection that hangs forever would leave the screener
// permanently un-warmed with no error anywhere. The timeout turns that into
// one failed symbol, which the gap path recovers.
inline constexpr auto kOperationTimeout = std::chrono::seconds(30);

class GetSession : public std::enable_shared_from_this<GetSession> {
   public:
    GetSession(net::io_context& ioc, ssl::context& ssl_ctx, std::string host, std::string port, std::string target,
               HttpResponseHandler handler, std::string log_target)
        : resolver_(ioc),
          stream_(ioc, ssl_ctx),
          host_(std::move(host)),
          port_(std::move(port)),
          target_(std::move(target)),
          // Declared after target_, so target_ is already initialised here.
          log_target_(log_target.empty() ? target_ : std::move(log_target)),
          handler_(std::move(handler)) {}

    // Not called from the constructor: shared_from_this() requires the object
    // to already be owned by a shared_ptr.
    void Run() {
        // SNI. api.bybit.com is behind a virtual host, so without this the
        // handshake fails with a certificate error that looks like a trust
        // problem rather than a missing header.
        if (!SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str())) {
            Fail("SNI", "SSL_set_tlsext_host_name failed");
            return;
        }

        req_.method(http::verb::get);
        req_.target(target_);
        req_.version(11);
        req_.set(http::field::host, host_);
        req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // NOTE: the resolver is not covered by the stream's timer - Beast's
        // timeout applies to stream operations only. A hung DNS lookup falls
        // back to the resolver's own timeout. Everything after this point is
        // bounded by kOperationTimeout.
        resolver_.async_resolve(host_, port_, beast::bind_front_handler(&GetSession::OnResolve, shared_from_this()));
    }

   private:
    void OnResolve(beast::error_code ec, const tcp::resolver::results_type& results) {
        if (ec) {
            Fail("resolve", ec.message());
            return;
        }
        beast::get_lowest_layer(stream_).expires_after(kOperationTimeout);
        beast::get_lowest_layer(stream_).async_connect(
            results, beast::bind_front_handler(&GetSession::OnConnect, shared_from_this()));
    }

    void OnConnect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type&) {
        if (ec) {
            Fail("connect", ec.message());
            return;
        }
        beast::get_lowest_layer(stream_).expires_after(kOperationTimeout);
        stream_.async_handshake(ssl::stream_base::client,
                                beast::bind_front_handler(&GetSession::OnHandshake, shared_from_this()));
    }

    void OnHandshake(beast::error_code ec) {
        if (ec) {
            Fail("handshake", ec.message());
            return;
        }
        beast::get_lowest_layer(stream_).expires_after(kOperationTimeout);
        http::async_write(stream_, req_, beast::bind_front_handler(&GetSession::OnWrite, shared_from_this()));
    }

    void OnWrite(beast::error_code ec, std::size_t) {
        if (ec) {
            Fail("write", ec.message());
            return;
        }
        beast::get_lowest_layer(stream_).expires_after(kOperationTimeout);
        http::async_read(stream_, buffer_, res_, beast::bind_front_handler(&GetSession::OnRead, shared_from_this()));
    }

    void OnRead(beast::error_code ec, std::size_t) {
        if (ec) {
            Fail("read", ec.message());
            return;
        }

        if (res_.result() != http::status::ok) {
            // 403 / retCode 10006 means the IP is rate-banned. The caller's
            // limiter is what prevents that; logging the status is how we
            // find out the limiter is set wrong.
            // The BODY, truncated, not just the status. Both services we
            // talk to explain a 4xx in it - Bybit with retMsg, Telegram with
            // "description" - and without it a 400 is unexplainable from the
            // log alone. Truncated because a non-200 body is occasionally an
            // HTML error page.
            constexpr std::size_t kMaxLoggedBody = 256;
            const std::string_view body(res_.body());
            Logger::Log(LogLevel::kError, "[HTTPS] GET {}{} returned status {}: {}{}", host_, log_target_,
                        res_.result_int(), body.substr(0, kMaxLoggedBody), body.size() > kMaxLoggedBody ? "..." : "");
            Complete(std::nullopt);
        } else {
            Complete(std::move(res_.body()));
        }

        // Hand the body to the caller FIRST, then shut down. The shutdown is
        // courtesy only - the response is already in hand, and most servers
        // close without a clean TLS shutdown anyway, so stream_truncated here
        // is normal and not reported.
        beast::get_lowest_layer(stream_).expires_after(kOperationTimeout);
        stream_.async_shutdown(beast::bind_front_handler(&GetSession::OnShutdown, shared_from_this()));
    }

    void OnShutdown(beast::error_code) {}

    void Fail(std::string_view what, std::string_view detail) {
        Logger::Log(LogLevel::kError, "[HTTPS] GET {}{} {} failed: {}", host_, log_target_, what, detail);
        Complete(std::nullopt);
    }

    // Moves the handler out before calling it, so a second completion path
    // cannot invoke it twice. Both OnRead and a later Fail could otherwise
    // reach it, and double-counting a warm-up response would make the
    // in-flight count drift and the warm-up never finish.
    void Complete(std::optional<std::string> body) {
        if (!handler_) {
            return;
        }
        HttpResponseHandler handler = std::move(handler_);
        handler_ = nullptr;
        handler(std::move(body));
    }

    tcp::resolver resolver_;
    beast::ssl_stream<beast::tcp_stream> stream_;

    std::string host_;
    std::string port_;
    std::string target_;

    // What the log prints instead of target_. Equal to target_ unless the
    // caller passed a redacted form - see the header.
    std::string log_target_;

    HttpResponseHandler handler_;

    http::request<http::empty_body> req_;
    http::response<http::string_body> res_;
    beast::flat_buffer buffer_;
};

}  // namespace

void AsyncHttpsGet(net::io_context& ioc, ssl::context& ssl_ctx, std::string host, std::string port, std::string target,
                   HttpResponseHandler handler, std::string log_target) {
    std::make_shared<GetSession>(ioc, ssl_ctx, std::move(host), std::move(port), std::move(target), std::move(handler),
                                 std::move(log_target))
        ->Run();
}

}  // namespace screener

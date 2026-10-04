// ────────────────────────────────────────────────────────────────────────────
// gateway.cpp  —  Single-Threaded WebSocket Gateway (Task 4.2)
//
// ARCHITECTURE
// ------------
// One Boost.Asio io_context, one thread.  The network event loop invokes the
// engine synchronously — no queues, locks, or threads needed.
//
// CONNECTION LIFECYCLE
//   1. TCP accept → WebSocket upgrade handshake
//   2. Async read loop: one JSON message at a time
//   3. Dispatch → engine / subscription handler
//   4. Write response(s) via async queue before next read
//   5. Graceful close on disconnect or protocol error
//
// MARKET DATA INTEGRATION
//   After every engine call, staged PublicTrade/BBO/L2Delta events are
//   broadcast to all sessions subscribed to the relevant channel.
//
// BACKPRESSURE
//   Each session has an outbound deque.  When it exceeds MAX_OUTBOUND_QUEUE
//   the session is dropped (the WebSocket is closed).
//
// THREAD SAFETY
//   Single-threaded: io_context::run() runs on exactly one thread.
//   g_sessions is accessed exclusively on that thread.
// ────────────────────────────────────────────────────────────────────────────

#include "orderbook/engine.hpp"
#include "orderbook/json_codec.hpp"
#include "orderbook/market_data.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <deque>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ws    = beast::websocket;
using     tcp   = asio::ip::tcp;

using nlohmann::json;

namespace orderbook {

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr std::size_t MAX_OUTBOUND_QUEUE = 256;
static constexpr std::size_t MAX_MESSAGE_BYTES  = 1'048'576;  // 1 MiB

// ── Channel name constants ────────────────────────────────────────────────────

static constexpr std::string_view CH_BBO      = "public.bbo";
static constexpr std::string_view CH_TRADES   = "public.trades";
static constexpr std::string_view CH_DEPTH    = "public.depth_l2";

// ── Session (forward-declared for broadcast) ──────────────────────────────────

class Session;
static std::set<Session*> g_sessions;   // single-threaded — no locking needed

// ── GatewayState (shared by all sessions) ─────────────────────────────────────

struct GatewayState {
    Engine         engine;
    MarketDataFeed feed;

    explicit GatewayState(const std::string& log_path)
        : engine(log_path)
    {}
};

// ── Staged broadcast buffers (filled per engine call, then broadcast) ──────────
// We collect outbound messages here before dispatching, to avoid iterator
// invalidation if a send causes a session to close.

struct BroadcastBatch {
    std::vector<std::string> trades;
    std::optional<std::string> bbo;
    std::vector<std::string> depth_deltas;
};

// ── Session ───────────────────────────────────────────────────────────────────

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(tcp::socket&& sock, GatewayState& state)
        : ws_(std::move(sock)), state_(state)
    {
        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator([](ws::response_type& res) {
            res.set(http::field::server, "orderbook-gateway/1.0");
        }));
    }

    ~Session() { g_sessions.erase(this); }

    void start() {
        g_sessions.insert(this);
        ws_.async_accept(beast::bind_front_handler(&Session::on_accept, shared_from_this()));
    }

    // Returns false if queue full (caller should close this session).
    bool enqueue(std::string msg) {
        if (outbound_.size() >= MAX_OUTBOUND_QUEUE) return false;
        outbound_.push_back(std::move(msg));
        if (!writing_) kick_write();
        return true;
    }

    bool subscribed_to(std::string_view ch) const {
        return channels_.contains(std::string(ch));
    }

    UserId user_id() const noexcept { return user_id_; }

private:
    // ── Handshake ─────────────────────────────────────────────────────────────

    void on_accept(beast::error_code ec) {
        if (ec) return log_fail(ec, "accept");
        do_read();
    }

    // ── Read loop ─────────────────────────────────────────────────────────────

    void do_read() {
        buf_.clear();
        ws_.async_read(buf_,
            beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t /*n*/) {
        if (ec == ws::error::closed) return;
        if (ec) return log_fail(ec, "read");

        if (buf_.size() > MAX_MESSAGE_BYTES) {
            send_system_error("", "MALFORMED_JSON", "Message exceeds maximum size");
            return graceful_close();
        }

        dispatch(beast::buffers_to_string(buf_.data()));
        // dispatch() is responsible for calling do_read() when done
    }

    // ── Dispatch ──────────────────────────────────────────────────────────────

    void dispatch(std::string text) {
        auto msg = codec::decode(text);
        std::visit([this](auto&& req) {
            using T = std::decay_t<decltype(req)>;
            if constexpr (std::is_same_v<T, codec::ParseError>)       handle_error(req);
            else if constexpr (std::is_same_v<T, codec::PlaceOrderReq>)    handle_place(req);
            else if constexpr (std::is_same_v<T, codec::CancelOrderReq>)   handle_cancel(req);
            else if constexpr (std::is_same_v<T, codec::SubscribeReq>)     handle_subscribe(req);
            else if constexpr (std::is_same_v<T, codec::GetSnapshotReq>)   handle_snapshot(req);
        }, msg);
        do_read();
    }

    // ── Parse error ───────────────────────────────────────────────────────────

    void handle_error(const codec::ParseError& e) {
        enqueue(codec::encode_parse_error(e));
    }

    // ── Place order ───────────────────────────────────────────────────────────

    void handle_place(const codec::PlaceOrderReq& req) {
        if (user_id_ == INVALID_USER_ID && req.user_id != INVALID_USER_ID)
            user_id_ = req.user_id;

        Order order{};
        order.user_id         = req.user_id;
        order.client_order_id = req.client_order_id;
        order.side            = req.side;
        order.type            = req.order_type;
        order.price           = req.price;
        order.quantity        = req.quantity;

        // Notify feed BEFORE calling engine so it knows the taker's original qty.
        // We use INVALID_ORDER_ID here; the engine will assign the real order_id
        // and emit OrderAccepted with it.  We update after.
        // NOTE: The feed needs notify_order_accepted(id, side, price, qty).
        // We do this inside the sink on OrderAccepted.

        Quantity   taker_remaining  = req.quantity;
        OrderId    taker_order_id   = INVALID_ORDER_ID;
        BroadcastBatch batch;

        auto sink = [&](const Event& ev) {
            switch (ev.tag()) {

            case EventTag::OrderAccepted: {
                taker_order_id = ev.accepted.order_id;
                // Tell the feed about this order (price + qty for depth tracking)
                state_.feed.notify_order_accepted(
                    taker_order_id, req.side, req.price, req.quantity);
                // Private ack to this session
                enqueue(codec::encode_order_accepted(
                    req.req_id, ev.accepted, req, ev.header.seq));
                break;
            }

            case EventTag::OrderRejected:
                enqueue(codec::encode_order_rejected(
                    req.req_id, ev.rejected, req, ev.header.seq));
                break;

            case EventTag::Fill: {
                taker_remaining = (taker_remaining >= ev.fill.quantity)
                    ? taker_remaining - ev.fill.quantity : 0;
                bool fully_filled = (taker_remaining == 0);

                // Private fill report to taker (this session)
                enqueue(codec::encode_execution_taker(
                    req.req_id, ev.fill, req,
                    taker_remaining, fully_filled, ev.header.seq));

                // Public market data via feed
                state_.feed.process(ev,
                    [&](const PublicTrade& t)  { batch.trades.push_back(codec::encode_trade(t)); },
                    [&](const BBO& b)           { batch.bbo = codec::encode_bbo(b); },
                    [&](const L2Update& deltas) {
                        for (const auto& d : deltas)
                            batch.depth_deltas.push_back(codec::encode_depth_update(d));
                    });
                break;
            }

            default: break;
            }
        };

        state_.engine.add(order, sink);

        // After all fills: flush pending resting order into feed
        state_.feed.flush_pending(
            [&](const BBO& b)           { batch.bbo = codec::encode_bbo(b); },
            [&](const L2Update& deltas) {
                for (const auto& d : deltas)
                    batch.depth_deltas.push_back(codec::encode_depth_update(d));
            });

        broadcast(batch);
    }

    // ── Cancel order ──────────────────────────────────────────────────────────

    void handle_cancel(const codec::CancelOrderReq& req) {
        ClientOrderId clord_id = req.client_order_id.value_or(INVALID_CLIENT_ORDER_ID);
        BroadcastBatch batch;

        auto sink = [&](const Event& ev) {
            switch (ev.tag()) {
            case EventTag::OrderCancelled:
                enqueue(codec::encode_order_cancelled(
                    req.req_id, ev.cancelled,
                    req.user_id, clord_id, ev.header.seq));
                state_.feed.process(ev,
                    [&](const PublicTrade&) {},
                    [&](const BBO& b)           { batch.bbo = codec::encode_bbo(b); },
                    [&](const L2Update& deltas) {
                        for (const auto& d : deltas)
                            batch.depth_deltas.push_back(codec::encode_depth_update(d));
                    });
                break;

            case EventTag::CancelRejected:
                enqueue(codec::encode_cancel_rejected(
                    req.req_id, ev.cancel_rejected, ev.header.seq));
                break;

            default: break;
            }
        };

        if (req.order_id.has_value())
            state_.engine.cancel(*req.order_id, req.user_id, sink);
        else if (req.client_order_id.has_value())
            state_.engine.cancel_by_client_id(req.user_id, *req.client_order_id, sink);

        broadcast(batch);
    }

    // ── Subscribe ─────────────────────────────────────────────────────────────

    void handle_subscribe(const codec::SubscribeReq& req) {
        bool new_depth = false;
        for (const auto& ch : req.channels) {
            if (!channels_.contains(ch) && ch == std::string(CH_DEPTH))
                new_depth = true;
            channels_.insert(ch);
        }

        json j = {
            {"req_id",  req.req_id},
            {"type",    "subscribed"},
            {"channel", "system"},
            {"status",  "ok"},
            {"payload", {{"channels", req.channels}}}
        };
        enqueue(j.dump());

        // Auto-send snapshot for new depth subscription
        if (new_depth) {
            auto snap = state_.feed.snapshot();
            enqueue(codec::encode_depth_snapshot(snap, ""));
        }
    }

    // ── Get snapshot ──────────────────────────────────────────────────────────

    void handle_snapshot(const codec::GetSnapshotReq& req) {
        if (req.channel == CH_DEPTH) {
            enqueue(codec::encode_depth_snapshot(state_.feed.snapshot(), req.req_id));
        } else {
            send_system_error(req.req_id, "MALFORMED_JSON",
                "Snapshots only available for public.depth_l2");
        }
    }

    // ── Broadcast market data to all subscribed sessions ──────────────────────

    static void broadcast(const BroadcastBatch& batch) {
        // Collect sessions to close (queue overflow) without touching the set
        std::vector<Session*> to_close;

        for (Session* s : g_sessions) {
            for (const auto& msg : batch.trades)
                if (s->subscribed_to(CH_TRADES) && !s->enqueue(msg))
                    { to_close.push_back(s); break; }

            if (batch.bbo && s->subscribed_to(CH_BBO))
                if (!s->enqueue(*batch.bbo))
                    to_close.push_back(s);

            for (const auto& msg : batch.depth_deltas)
                if (s->subscribed_to(CH_DEPTH) && !s->enqueue(msg))
                    { to_close.push_back(s); break; }
        }

        for (Session* s : to_close) {
            // Schedule close on the executor (can't close while iterating)
            asio::post(s->ws_.get_executor(),
                [s_weak = s->weak_from_this()]() {
                    if (auto s = s_weak.lock()) s->graceful_close();
                });
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    void send_system_error(const std::string& req_id,
                           const std::string& code,
                           const std::string& msg) {
        enqueue(codec::encode_parse_error({req_id, code, msg}));
    }

    void graceful_close() {
        if (!ws_.is_open()) return;
        ws_.async_close(ws::close_code::normal,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec && ec != ws::error::closed)
                    std::cerr << "[session] close: " << ec.message() << "\n";
            });
    }

    void log_fail(beast::error_code ec, std::string_view where) {
        if (ec == ws::error::closed ||
            ec == asio::error::connection_reset ||
            ec == asio::error::eof)
            return;
        std::cerr << "[session] " << where << ": " << ec.message() << "\n";
    }

    // ── Write queue ───────────────────────────────────────────────────────────

    void kick_write() {
        if (outbound_.empty()) { writing_ = false; return; }
        writing_ = true;
        ws_.async_write(asio::buffer(outbound_.front()),
            beast::bind_front_handler(&Session::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t) {
        if (ec) return log_fail(ec, "write");
        outbound_.pop_front();
        kick_write();
    }

    // ── Members ───────────────────────────────────────────────────────────────

    ws::stream<tcp::socket>        ws_;
    beast::flat_buffer             buf_;
    GatewayState&                  state_;
    std::set<std::string>          channels_;
    UserId                         user_id_{INVALID_USER_ID};
    std::deque<std::string>        outbound_;
    bool                           writing_{false};
};

// ── Listener ──────────────────────────────────────────────────────────────────

class Listener : public std::enable_shared_from_this<Listener> {
public:
    Listener(asio::io_context& ioc, tcp::endpoint ep, GatewayState& state)
        : ioc_(ioc), acceptor_(ioc), state_(state)
    {
        beast::error_code ec;
        acceptor_.open(ep.protocol(), ec);   if (ec) throw_ec(ec, "open");
        acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
        acceptor_.bind(ep, ec);              if (ec) throw_ec(ec, "bind");
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) throw_ec(ec, "listen");
    }

    void run() { do_accept(); }

private:
    void do_accept() {
        acceptor_.async_accept(
            beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket sock) {
        if (!ec)
            std::make_shared<Session>(std::move(sock), state_)->start();
        else
            std::cerr << "[listener] accept: " << ec.message() << "\n";
        do_accept();
    }

    [[noreturn]] static void throw_ec(beast::error_code ec, std::string_view what) {
        throw std::runtime_error(std::string(what) + ": " + ec.message());
    }

    asio::io_context& ioc_;
    tcp::acceptor     acceptor_;
    GatewayState&     state_;
};

} // namespace orderbook

// ── Entry point ───────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    using namespace orderbook;

    uint16_t    port     = 9001;
    std::string log_path = "gateway.log";
    std::string host     = "0.0.0.0";

    if (argc >= 2) port     = static_cast<uint16_t>(std::stoi(argv[1]));
    if (argc >= 3) log_path = argv[2];
    if (argc >= 4) host     = argv[3];

    std::cout << "[gateway] starting  ws://" << host << ":" << port
              << "  log=" << log_path << "\n";

    try {
        asio::io_context ioc{1};

        GatewayState state(log_path);

        auto ep = tcp::endpoint{asio::ip::make_address(host), port};
        std::make_shared<Listener>(ioc, ep, state)->run();

        asio::signal_set sigs(ioc, SIGINT, SIGTERM);
        sigs.async_wait([&](beast::error_code, int sig) {
            std::cout << "\n[gateway] signal " << sig << " — shutting down\n";
            state.engine.flush();
            ioc.stop();
        });

        std::cout << "[gateway] listening — Ctrl+C to stop\n";
        ioc.run();

    } catch (const std::exception& e) {
        std::cerr << "[gateway] fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

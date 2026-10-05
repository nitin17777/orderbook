#pragma once

// ────────────────────────────────────────────────────────────────────────────
// json_codec.hpp  —  JSON serialization / deserialization for the gateway
//
// DESIGN
// ------
// This layer sits strictly BETWEEN the network and the engine.  It converts:
//   • Inbound JSON text → validated engine Commands / subscriptions
//   • Outbound engine Events → JSON text per the wire_protocol.md spec
//
// The codec has NO dependencies on the network layer (no Boost headers).
// It depends only on nlohmann::json and the orderbook domain types.
//
// PRIVATE vs PUBLIC BOUNDARY
// --------------------------
// Encoder functions for public market data channels (encode_trade,
// encode_bbo, encode_depth_*) produce messages with NO user_id or
// order_id fields — enforced by only accepting PublicTrade/BBO/L2*
// types, which are already sanitized structs (see market_data.hpp).
//
// SEQUENCE NUMBERS
// ----------------
// The codec is stateless with respect to sequencing.  Callers pass
// the seq number directly.  The gateway owns and advances the counter.
// ────────────────────────────────────────────────────────────────────────────

#include "orderbook/engine.hpp"
#include "orderbook/market_data.hpp"
#include "orderbook/types.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orderbook {
namespace codec {

using json = nlohmann::json;

// ── Inbound request types ─────────────────────────────────────────────────────

struct PlaceOrderReq {
    std::string     req_id;
    UserId          user_id{INVALID_USER_ID};
    ClientOrderId   client_order_id{INVALID_CLIENT_ORDER_ID};
    Side            side{Side::Buy};
    OrderType       order_type{OrderType::Limit};
    TimeInForce     tif{TimeInForce::GTC};
    Price           price{0};
    Quantity        quantity{0};
};

struct CancelOrderReq {
    std::string     req_id;
    UserId          user_id{INVALID_USER_ID};
    // exactly one of order_id or client_order_id will be valid
    std::optional<OrderId>       order_id{};
    std::optional<ClientOrderId> client_order_id{};
};

struct ModifyOrderReq {
    std::string                  req_id;
    UserId                       user_id{INVALID_USER_ID};
    std::optional<OrderId>       order_id{};
    std::optional<ClientOrderId> client_order_id{};
    Price                        new_price{0};
    Quantity                     new_quantity{0};
};

struct SubscribeReq {
    std::string              req_id;
    std::vector<std::string> channels;
};

struct GetSnapshotReq {
    std::string req_id;
    std::string channel;
};

struct ParseError {
    std::string req_id;    // may be empty if envelope itself was malformed
    std::string code;
    std::string message;
};

using InboundMessage = std::variant<
    PlaceOrderReq,
    CancelOrderReq,
    ModifyOrderReq,
    SubscribeReq,
    GetSnapshotReq,
    ParseError
>;

// ── JSON helpers ─────────────────────────────────────────────────────────────

inline std::string side_str(Side s) {
    return s == Side::Buy ? "buy" : "sell";
}

inline std::string order_type_str(OrderType t) {
    return t == OrderType::Limit ? "limit" : "market";
}

inline std::string time_in_force_str(TimeInForce t) {
    switch (t) {
        case TimeInForce::GTC: return "gtc";
        case TimeInForce::IOC: return "ioc";
        case TimeInForce::FOK: return "fok";
    }
    return "gtc";
}

inline std::string reject_code(RejectReason r) {
    switch (r) {
        case RejectReason::InvalidPrice:           return "INVALID_PRICE";
        case RejectReason::InvalidQuantity:        return "INVALID_QUANTITY";
        case RejectReason::DuplicateOrderId:       return "DUPLICATE_ORDER_ID";
        case RejectReason::DuplicateClientOrderId: return "DUPLICATE_CLIENT_ORDER_ID";
        case RejectReason::UnknownOrder:           return "UNKNOWN_ORDER";
        case RejectReason::Unauthorized:           return "UNAUTHORIZED";
        case RejectReason::InvalidSide:            return "INVALID_SIDE";
        case RejectReason::InvalidType:            return "INVALID_ORDER_TYPE";
        case RejectReason::InvalidTimeInForce:     return "INVALID_TIME_IN_FORCE";
        default:                                   return "UNKNOWN";
    }
}

inline std::string reject_msg(RejectReason r) {
    switch (r) {
        case RejectReason::InvalidPrice:           return "Limit price must be strictly positive";
        case RejectReason::InvalidQuantity:        return "Quantity must be > 0";
        case RejectReason::DuplicateOrderId:       return "Order ID already in use";
        case RejectReason::DuplicateClientOrderId: return "Client order ID already in use";
        case RejectReason::UnknownOrder:           return "Order ID not found or already terminal";
        case RejectReason::Unauthorized:           return "User ID does not own this order";
        case RejectReason::InvalidSide:            return "Side must be 'buy' or 'sell'";
        case RejectReason::InvalidType:            return "Order type must be 'limit' or 'market'";
        case RejectReason::InvalidTimeInForce:     return "Time in force must be 'gtc', 'ioc', or 'fok'";
        default:                                   return "Unknown error";
    }
}

// ── Inbound decoder ──────────────────────────────────────────────────────────

inline InboundMessage decode(std::string_view text) {
    std::string req_id;
    try {
        auto j = json::parse(text);

        req_id = j.value("req_id", "");
        std::string type = j.at("type").get<std::string>();

        // ── place_order ──────────────────────────────────────────────────────
        if (type == "place_order") {
            const auto& p = j.at("payload");
            PlaceOrderReq req;
            req.req_id = req_id;
            req.user_id         = p.at("user_id").get<UserId>();
            req.client_order_id = p.at("client_order_id").get<ClientOrderId>();

            std::string side_s = p.at("side").get<std::string>();
            if      (side_s == "buy")  req.side = Side::Buy;
            else if (side_s == "sell") req.side = Side::Sell;
            else return ParseError{req_id, "INVALID_SIDE", "Side must be 'buy' or 'sell'"};

            std::string ot = p.at("order_type").get<std::string>();
            if      (ot == "limit")  req.order_type = OrderType::Limit;
            else if (ot == "market") req.order_type = OrderType::Market;
            else return ParseError{req_id, "INVALID_ORDER_TYPE", "Order type must be 'limit' or 'market'"};

            if (p.contains("time_in_force")) {
                std::string tif_s = p.at("time_in_force").get<std::string>();
                if      (tif_s == "gtc" || tif_s == "GTC") req.tif = TimeInForce::GTC;
                else if (tif_s == "ioc" || tif_s == "IOC") req.tif = TimeInForce::IOC;
                else if (tif_s == "fok" || tif_s == "FOK") req.tif = TimeInForce::FOK;
                else return ParseError{req_id, "INVALID_TIME_IN_FORCE", "time_in_force must be 'gtc', 'ioc', or 'fok'"};
            } else if (p.contains("tif")) {
                std::string tif_s = p.at("tif").get<std::string>();
                if      (tif_s == "gtc" || tif_s == "GTC") req.tif = TimeInForce::GTC;
                else if (tif_s == "ioc" || tif_s == "IOC") req.tif = TimeInForce::IOC;
                else if (tif_s == "fok" || tif_s == "FOK") req.tif = TimeInForce::FOK;
                else return ParseError{req_id, "INVALID_TIME_IN_FORCE", "time_in_force must be 'gtc', 'ioc', or 'fok'"};
            }

            req.price    = p.value("price", Price{0});
            req.quantity = p.at("quantity").get<Quantity>();
            return req;
        }

        // ── cancel_order ─────────────────────────────────────────────────────
        if (type == "cancel_order") {
            const auto& p = j.at("payload");
            CancelOrderReq req;
            req.req_id  = req_id;
            req.user_id = p.at("user_id").get<UserId>();

            if (p.contains("order_id"))
                req.order_id = p["order_id"].get<OrderId>();
            else if (p.contains("client_order_id"))
                req.client_order_id = p["client_order_id"].get<ClientOrderId>();
            else
                return ParseError{req_id, "MALFORMED_JSON", "cancel_order requires order_id or client_order_id"};

            return req;
        }

        // ── modify_order ─────────────────────────────────────────────────────
        if (type == "modify_order") {
            const auto& p = j.at("payload");
            ModifyOrderReq req;
            req.req_id  = req_id;
            req.user_id = p.at("user_id").get<UserId>();

            if (p.contains("order_id"))
                req.order_id = p["order_id"].get<OrderId>();
            else if (p.contains("client_order_id"))
                req.client_order_id = p["client_order_id"].get<ClientOrderId>();
            else
                return ParseError{req_id, "MALFORMED_JSON", "modify_order requires order_id or client_order_id"};

            req.new_price    = p.at("new_price").get<Price>();
            req.new_quantity = p.at("new_quantity").get<Quantity>();
            return req;
        }

        // ── subscribe ────────────────────────────────────────────────────────
        if (type == "subscribe") {
            SubscribeReq req;
            req.req_id   = req_id;
            req.channels = j.at("payload").at("channels").get<std::vector<std::string>>();
            return req;
        }

        // ── get_snapshot ─────────────────────────────────────────────────────
        if (type == "get_snapshot") {
            GetSnapshotReq req;
            req.req_id  = req_id;
            req.channel = j.at("payload").at("channel").get<std::string>();
            return req;
        }

        return ParseError{req_id, "MALFORMED_JSON", "Unknown message type: " + type};

    } catch (const json::exception& ex) {
        return ParseError{req_id, "MALFORMED_JSON", ex.what()};
    } catch (const std::exception& ex) {
        return ParseError{req_id, "MALFORMED_JSON", ex.what()};
    }
}

// ── Outbound encoders (private execution reports) ─────────────────────────────

inline std::string encode_parse_error(const ParseError& e) {
    json j = {
        {"type",    "error"},
        {"channel", "system"},
        {"status",  "error"},
        {"error",   {{"code", e.code}, {"message", e.message}}}
    };
    if (!e.req_id.empty()) j["req_id"] = e.req_id;
    return j.dump();
}

// Called after engine emits OrderAccepted event.
// We need the original PlaceOrderReq to echo back user & client order id.
inline std::string encode_order_accepted(
    const std::string& req_id,
    const EventOrderAccepted& ev,
    const PlaceOrderReq& req,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "order_accepted"},
        {"channel",   "private.orders"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "ok"},
        {"payload",   {
            {"order_id",        ev.order_id},
            {"client_order_id", req.client_order_id},
            {"user_id",         req.user_id},
            {"side",            side_str(ev.side)},
            {"order_type",      order_type_str(ev.order_type)},
            {"time_in_force",   time_in_force_str(ev.tif)},
            {"price",           req.price},
            {"quantity",        req.quantity},
            {"status",          "accepted"}
        }}
    };
    return j.dump();
}

inline std::string encode_order_rejected(
    const std::string& req_id,
    const EventOrderRejected& ev,
    const PlaceOrderReq& req,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "order_rejected"},
        {"channel",   "private.orders"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "error"},
        {"error",     {{"code", reject_code(ev.reason)}, {"message", reject_msg(ev.reason)}}},
        {"payload",   {
            {"client_order_id", req.client_order_id},
            {"user_id",         req.user_id}
        }}
    };
    return j.dump();
}

// Per-fill execution report for the TAKER (the submitting user).
// resting_order_id is needed to look up remaining qty; we pass it explicitly.
inline std::string encode_execution_taker(
    const std::string& req_id,
    const EventFill& ev,
    const PlaceOrderReq& req,
    Quantity remaining_qty,
    bool fully_filled,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "execution"},
        {"channel",   "private.fills"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "ok"},
        {"payload",   {
            {"order_id",        ev.taker_id},
            {"client_order_id", req.client_order_id},
            {"user_id",         req.user_id},
            {"side",            side_str(req.side)},
            {"role",            "taker"},
            {"match_price",     ev.price},
            {"filled_qty",      ev.quantity},
            {"remaining_qty",   remaining_qty},
            {"order_status",    fully_filled ? "filled" : "partially_filled"}
        }}
    };
    return j.dump();
}

inline std::string encode_order_cancelled(
    const std::string& req_id,
    const EventOrderCancelled& ev,
    UserId user_id,
    ClientOrderId clord_id,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "order_cancelled"},
        {"channel",   "private.orders"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "ok"},
        {"payload",   {
            {"order_id",        ev.order_id},
            {"client_order_id", clord_id},
            {"user_id",         user_id},
            {"remaining_qty",   ev.remaining_qty},
            {"status",          "cancelled"}
        }}
    };
    return j.dump();
}

inline std::string encode_order_modified(
    const std::string& req_id,
    const EventOrderModified& ev,
    UserId user_id,
    ClientOrderId clord_id,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "order_modified"},
        {"channel",   "private.orders"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "ok"},
        {"payload",   {
            {"order_id",        ev.order_id},
            {"client_order_id", clord_id},
            {"user_id",         user_id},
            {"side",            side_str(ev.side)},
            {"new_price",       ev.new_price},
            {"new_quantity",    ev.new_quantity},
            {"status",          "modified"}
        }}
    };
    return j.dump();
}

inline std::string encode_cancel_rejected(
    const std::string& req_id,
    const EventCancelRejected& ev,
    uint64_t seq)
{
    json j = {
        {"req_id",    req_id},
        {"type",      "cancel_rejected"},
        {"channel",   "private.orders"},
        {"seq",       seq},
        {"timestamp", ev.header.engine_time},
        {"status",    "error"},
        {"error",     {{"code", reject_code(ev.reason)}, {"message", reject_msg(ev.reason)}}}
    };
    return j.dump();
}

// ── Outbound encoders (public market data) ────────────────────────────────────
// These functions accept ONLY sanitized public types — no user/order IDs leak.

inline std::string encode_trade(const PublicTrade& t) {
    json j = {
        {"type",      "trade"},
        {"channel",   "public.trades"},
        {"seq",       t.seq},
        {"timestamp", t.timestamp},
        {"status",    "ok"},
        {"payload",   {
            {"price",          t.price},
            {"quantity",       t.quantity},
            {"aggressor_side", side_str(t.aggressor_side)}
        }}
    };
    return j.dump();
}

inline std::string encode_bbo(const BBO& b) {
    json payload;
    if (b.has_bid()) {
        payload["bid_price"] = *b.bid_price;
        payload["bid_qty"]   = b.bid_qty;
    } else {
        payload["bid_price"] = nullptr;
        payload["bid_qty"]   = 0;
    }
    if (b.has_ask()) {
        payload["ask_price"] = *b.ask_price;
        payload["ask_qty"]   = b.ask_qty;
    } else {
        payload["ask_price"] = nullptr;
        payload["ask_qty"]   = 0;
    }
    if (b.spread())    payload["spread"]    = *b.spread();
    else               payload["spread"]    = nullptr;
    if (b.mid_price()) payload["mid_price"] = *b.mid_price();
    else               payload["mid_price"] = nullptr;

    json j = {
        {"type",    "bbo"},
        {"channel", "public.bbo"},
        {"seq",     b.seq},
        {"status",  "ok"},
        {"payload", payload}
    };
    return j.dump();
}

inline std::string encode_depth_snapshot(const L2Snapshot& snap, const std::string& req_id = "") {
    json bids = json::array();
    json asks = json::array();
    for (const auto& lvl : snap.bids) bids.push_back({{"price", lvl.price}, {"qty", lvl.qty}});
    for (const auto& lvl : snap.asks) asks.push_back({{"price", lvl.price}, {"qty", lvl.qty}});

    json j = {
        {"type",    "depth_snapshot"},
        {"channel", "public.depth_l2"},
        {"seq",     snap.seq},
        {"status",  "ok"},
        {"payload", {{"bids", bids}, {"asks", asks}}}
    };
    if (!req_id.empty()) j["req_id"] = req_id;
    return j.dump();
}

inline std::string encode_depth_update(const L2Delta& d) {
    json j = {
        {"type",     "depth_update"},
        {"channel",  "public.depth_l2"},
        {"seq",      d.seq},
        {"prev_seq", d.prev_seq},
        {"status",   "ok"},
        {"payload",  {
            {"side",    side_str(d.side)},
            {"price",   d.price},
            {"new_qty", d.new_qty}
        }}
    };
    return j.dump();
}

} // namespace codec
} // namespace orderbook

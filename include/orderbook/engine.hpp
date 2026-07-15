#pragma once

#include "orderbook/book.hpp"
#include "orderbook/event_log.hpp"

#include <vector>
#include <string>

namespace orderbook {

// Engine wraps OrderBook with an EventLog.
// Every command is logged before being applied.
// The book can be reconstructed from the log at any time via replay().

class Engine {
public:
    explicit Engine(const std::string& log_path)
        : log_(log_path)
    {}

    std::vector<Fill> add(Order order) {
        log_.append(Command::add(order));
        return book_.add(order);
    }

    bool cancel(OrderId id) {
        log_.append(Command::cancel(id));
        return book_.cancel(id);
    }

    void flush() { log_.flush(); }

    const OrderBook& book()     const { return book_; }
    const std::string& log_path() const { return log_.path(); }

    // Replay a log file into a fresh OrderBook.
    // Returns the reconstructed book and the fill history.
    struct ReplayResult {
        OrderBook          book;
        std::vector<Fill>  fills;
    };

    static ReplayResult replay(const std::string& log_path) {
        auto commands = EventLog::read_all(log_path);

        ReplayResult result;
        for (const auto& cmd : commands) {
            if (cmd.type == CommandType::AddOrder) {
                auto fills = result.book.add(cmd.order);
                result.fills.insert(result.fills.end(),
                                    fills.begin(), fills.end());
            } else if (cmd.type == CommandType::CancelOrder) {
                result.book.cancel(cmd.cancel_id);
            }
        }
        return result;
    }

private:
    EventLog  log_;
    OrderBook book_;
};

} // namespace orderbook
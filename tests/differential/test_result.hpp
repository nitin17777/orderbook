#pragma once

#include "orderbook/fill.hpp"
#include <vector>
#include <ostream>

namespace orderbook::test {

struct CommandResult {
    bool              success{false};
    std::vector<Fill> fills{};

    bool operator==(const CommandResult& other) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const Fill& f) {
    os << "{taker=" << f.taker_id
       << ", maker=" << f.maker_id
       << ", price=" << f.price
       << ", qty="   << f.quantity << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const CommandResult& res) {
    os << "Result[success=" << (res.success ? "true" : "false")
       << ", fills=" << res.fills.size() << " {";
    for (size_t i = 0; i < res.fills.size(); ++i) {
        if (i > 0) os << ", ";
        os << res.fills[i];
    }
    os << "}]";
    return os;
}

} // namespace orderbook::test

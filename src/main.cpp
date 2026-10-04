#include "orderbook/engine.hpp"
#include "orderbook/event_log.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace orderbook;

// ── Display helpers ───────────────────────────────────────────────────────────

static void print_help() {
  std::cout << "\nCommands:\n"
            << "  add buy  <price> <qty>   - submit a limit buy order\n"
            << "  add sell <price> <qty>   - submit a limit sell order\n"
            << "  mkt buy  <qty>           - submit a market buy order\n"
            << "  mkt sell <qty>           - submit a market sell order\n"
            << "  cancel <id>              - cancel a resting order\n"
            << "  book                     - display current order book\n"
            << "  bbo                      - display Best Bid / Offer (BBO) & spread\n"
            << "  log                      - inspect all persisted WAL commands on disk\n"
            << "  help                     - show this message\n"
            << "  quit                     - exit\n\n";
}

static void print_fills(const std::vector<Fill> &fills) {
  for (const auto &f : fills) {
    std::cout << "  FILL  maker=" << f.maker_id << " taker=" << f.taker_id
              << " price=" << f.price << " qty=" << f.quantity << "\n";
  }
}

static void print_log_file(const std::string& path) {
  try {
    auto commands = EventLog::read_all(path);
    std::cout << "\n=== Binary WAL Log: " << path << " (" << commands.size() << " commands) ===\n";
    if (commands.empty()) {
      std::cout << "  (Log is currently empty)\n\n";
      return;
    }
    for (size_t i = 0; i < commands.size(); ++i) {
      const auto& c = commands[i];
      std::cout << "  [" << std::setw(3) << i << "] ";
      if (c.type == CommandType::AddOrder) {
        std::cout << "ADD    ID=" << std::setw(3) << c.order.id << " "
                  << (c.order.side == Side::Buy ? "BUY " : "SELL") << " "
                  << (c.order.type == OrderType::Limit ? "LIMIT " : "MKT   ")
                  << std::setw(4) << c.order.quantity << " @ "
                  << std::setw(4) << c.order.price
                  << " (ts=" << c.order.timestamp << ")";
      } else {
        std::cout << "CANCEL ID=" << std::setw(3) << c.cancel_id;
      }
      std::cout << "\n";
    }
    std::cout << "========================================================\n\n";
  } catch (const std::exception& e) {
    std::cout << "  Cannot read log: " << e.what() << "\n\n";
  }
}

static void print_book(const OrderBook &book) {
  const auto &bids = book.bids();
  const auto &asks = book.asks();

  std::cout << "\n"
            << std::setw(20) << std::left << "BID" << std::setw(20)
            << std::right << "ASK" << "\n"
            << std::string(40, '-') << "\n";

  auto bid_it = bids.begin();
  auto ask_it = asks.begin();

  int levels = 0;
  while ((bid_it != bids.end() || ask_it != asks.end()) && levels < 8) {
    std::string bid_str, ask_str;

    if (bid_it != bids.end()) {
      Quantity total = 0;
      for (const auto &o : bid_it->second)
        total += o.open_quantity();
      bid_str = std::to_string(bid_it->first) + " [" + std::to_string(total) + "]";
      ++bid_it;
    }

    if (ask_it != asks.end()) {
      Quantity total = 0;
      for (const auto &o : ask_it->second)
        total += o.open_quantity();
      ask_str = "[" + std::to_string(total) + "] " + std::to_string(ask_it->first);
      ++ask_it;
    }

    std::cout << std::setw(20) << std::left << bid_str << std::setw(20)
              << std::right << ask_str << "\n";
    ++levels;
  }
  std::cout << "\n";
}

// ── Order id counter ──────────────────────────────────────────────────────────

static OrderId next_id = 1;

static Order make_limit(Side side, Price price, Quantity qty) {
  Order o{};
  o.id = next_id++;
  o.side = side;
  o.type = OrderType::Limit;
  o.status = OrderStatus::Accepted;
  o.price = price;
  o.quantity = qty;
  o.filled = 0;
  o.timestamp = o.id;
  return o;
}

static Order make_market(Side side, Quantity qty) {
  Order o{};
  o.id = next_id++;
  o.side = side;
  o.type = OrderType::Market;
  o.status = OrderStatus::Accepted;
  o.price = 0;
  o.quantity = qty;
  o.filled = 0;
  o.timestamp = o.id;
  return o;
}

// ── Main loop ─────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
  if (argc > 2 && std::string(argv[1]) == "--dump") {
    print_log_file(argv[2]);
    return 0;
  }

  std::string log_path = "orderbook.log";
  if (argc > 1 && std::string(argv[1]) != "--dump")
    log_path = argv[1];

  Engine engine(log_path);

  std::cout << "OrderBook Engine - WAL file: " << log_path << "\n";
  std::cout << "Type 'help' for commands, 'log' to inspect WAL on disk.\n\n";

  std::string line;
  while (true) {
    std::cout << "> ";
    if (!std::getline(std::cin, line))
      break;

    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;

    if (cmd == "quit" || cmd == "exit") {
      engine.flush();
      std::cout << "Log saved to " << log_path << ". Goodbye.\n";
      break;
    }

    if (cmd == "help") {
      print_help();
      continue;
    }

    if (cmd == "book") {
      print_book(engine.book());
      continue;
    }

    if (cmd == "bbo") {
      auto bid = engine.book().best_bid();
      auto ask = engine.book().best_ask();
      std::cout << "\n=== BBO (Best Bid / Offer) ===\n";
      if (bid) std::cout << "  Best Bid: " << *bid << " (qty: " << engine.book().bid_level_qty(*bid) << ")\n";
      else     std::cout << "  Best Bid: (none)\n";
      if (ask) std::cout << "  Best Ask: " << *ask << " (qty: " << engine.book().ask_level_qty(*ask) << ")\n";
      else     std::cout << "  Best Ask: (none)\n";
      if (bid && ask) {
        std::cout << "  Spread:   " << (*ask - *bid) << "\n";
        std::cout << "  Mid:      " << ((*bid + *ask) / 2.0) << "\n";
      }
      std::cout << "\n";
      continue;
    }

    if (cmd == "log") {
      engine.flush();
      print_log_file(log_path);
      continue;
    }

    if (cmd == "cancel") {
      OrderId id;
      if (!(ss >> id)) {
        std::cout << "  usage: cancel <id>\n";
        continue;
      }
      auto res = engine.cancel(id);
      if (res.accepted)
        std::cout << "  [ORDER " << id << "] cancelled\n";
      else
        std::cout << "  [ORDER " << id
                  << "] rejected: " << to_string(res.reason) << "\n";
      continue;
    }

    if (cmd == "add") {
      std::string side_str;
      Price price;
      Quantity qty;
      if (!(ss >> side_str >> price >> qty)) {
        std::cout << "  usage: add buy|sell <price> <qty>\n";
        continue;
      }
      if (side_str != "buy" && side_str != "sell") {
        std::cout << "  unknown side '" << side_str << "' — use buy or sell\n";
        continue;
      }
      Side side = (side_str == "buy") ? Side::Buy : Side::Sell;
      Order o = make_limit(side, price, qty);
      OrderId assigned_id = o.id;
      auto res = engine.add(o);

      if (!res.accepted) {
        std::cout << "  [ORDER " << assigned_id
                  << "] REJECTED: " << to_string(res.reason) << "\n";
        continue;
      }

      std::cout << "  [ORDER " << assigned_id << "] " << side_str << " LIMIT "
                << qty << "@" << price;
      if (res.fills.empty())
        std::cout << " resting\n";
      else {
        std::cout << "\n";
        print_fills(res.fills);
      }
      continue;
    }

    if (cmd == "mkt") {
      std::string side_str;
      Quantity qty;
      if (!(ss >> side_str >> qty)) {
        std::cout << "  usage: mkt buy|sell <qty>\n";
        continue;
      }
      if (side_str != "buy" && side_str != "sell") {
        std::cout << "  unknown side '" << side_str << "' — use buy or sell\n";
        continue;
      }
      Side side = (side_str == "buy") ? Side::Buy : Side::Sell;
      Order o = make_market(side, qty);
      OrderId assigned_id = o.id;
      auto res = engine.add(o);

      if (!res.accepted) {
        std::cout << "  [ORDER " << assigned_id
                  << "] REJECTED: " << to_string(res.reason) << "\n";
        continue;
      }

      std::cout << "  [ORDER " << assigned_id << "] " << side_str << " MARKET "
                << qty;
      if (res.fills.empty())
        std::cout << " — no liquidity\n";
      else {
        std::cout << "\n";
        print_fills(res.fills);
      }
      continue;
    }

    if (!cmd.empty())
      std::cout << "  unknown command: '" << cmd << "'. Type 'help'.\n";
  }

  return 0;
}
#pragma once

#include "event.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace marketstate {

struct Order {
  OrderId order_id = 0;
  Side side = Side::BID;
  Price price = 0;
  Quantity quantity = 0;
  SeqNum last_seq = 0;
  bool active = true;
};

struct PriceLevel {
  Price price = 0;
  Quantity total_quantity = 0;
};

struct BookSnapshot {
  std::string symbol;
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  Quantity total_volume = 0;
  Quantity total_trades = 0;
  SeqNum last_applied_seq = 0;
};

// Deterministic limit order book. Price levels are maintained incrementally
// (O(log n) per update) instead of rebuilding from all orders on every event.
class OrderBook {
public:
  explicit OrderBook(std::string symbol = "");

  // Apply one already-ordered, already-validated event. Returns false only for
  // events that target a different symbol. Structural no-ops (e.g. cancel of an
  // unknown order) are deterministic and return true.
  bool apply_event(const Event& event);

  BookSnapshot snapshot() const;
  std::string canonical_state() const;

  // Restore from a canonical_state() string. Returns false on malformed input,
  // leaving the book cleared rather than partially applied.
  bool load_canonical_state(const std::string& state);

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  std::optional<Price> spread() const;      // ask - bid, if both present
  std::optional<double> midpoint() const;   // only used for display

  const Order* find_order(OrderId id) const;
  const std::unordered_map<OrderId, Order>& orders() const { return orders_; }
  const std::map<Price, PriceLevel, std::greater<Price>>& bids() const { return bids_; }
  const std::map<Price, PriceLevel>& asks() const { return asks_; }

  Quantity total_volume() const { return total_volume_; }
  Quantity total_trades() const { return total_trades_; }
  SeqNum last_applied_seq() const { return last_applied_seq_; }
  const std::string& symbol() const { return symbol_; }

  void clear();

private:
  void add_order(const Event& event, const OrderAdd& add);
  void cancel_order(const Event& event, const OrderCancel& cancel);
  void modify_order(const Event& event, const OrderModify& modify);
  void process_trade(const Event& event, const Trade& trade);

  void level_remove(Side side, Price price, Quantity qty);
  void level_add(Side side, Price price, Quantity qty);

  std::string symbol_;
  std::unordered_map<OrderId, Order> orders_;
  std::map<Price, PriceLevel, std::greater<Price>> bids_;
  std::map<Price, PriceLevel> asks_;
  Quantity total_volume_ = 0;
  Quantity total_trades_ = 0;
  SeqNum last_applied_seq_ = 0;
};

} // namespace marketstate

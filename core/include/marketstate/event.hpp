#pragma once

#include "types.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace marketstate {

enum class EventType : int {
  ORDER_ADD = 0,
  ORDER_CANCEL = 1,
  ORDER_MODIFY = 2,
  TRADE = 3,
  MARKET_UPDATE = 4,
};

enum class Side : int {
  BID = 0,
  ASK = 1,
};

struct OrderAdd {
  OrderId order_id = 0;
  Side side = Side::BID;
  Price price = 0;
  Quantity quantity = 0;
};

struct OrderCancel {
  OrderId order_id = 0;
  // kNoQuantity => cancel the full remaining quantity.
  Quantity quantity = kNoQuantity;
};

struct OrderModify {
  OrderId order_id = 0;
  // kNoPrice / kNoQuantity mean "leave unchanged".
  Price new_price = kNoPrice;
  Quantity new_quantity = kNoQuantity;
};

struct Trade {
  OrderId buy_order_id = 0;
  OrderId sell_order_id = 0;
  Price price = 0;
  Quantity quantity = 0;
};

struct MarketUpdate {
  Price best_bid = kNoPrice;
  Price best_ask = kNoPrice;
};

using Payload = std::variant<OrderAdd, OrderCancel, OrderModify, Trade, MarketUpdate>;

// A single market event. `checksum` protects integrity; `valid`/`error` carry
// the outcome of validation performed by the ingestion path.
struct Event {
  EventId event_id = 0;
  StreamId stream_id = 0;
  SeqNum sequence_num = 0;
  std::string symbol;
  EventType type = EventType::ORDER_ADD;
  Timestamp timestamp = 0;
  uint64_t checksum = 0;
  Payload payload = OrderAdd{};

  bool valid = true;
  std::string error;
};

// ---- Canonical text encoding -------------------------------------------------
// One event per line:
//   event_id stream_id seq symbol type timestamp <payload...> checksum
// The checksum trails the record and covers every preceding field. The format
// is shared with the Python generator and both reference models.
std::string encode_event(const Event& event);

// Parse a canonical line. Returns std::nullopt only for structurally
// unparseable input; checksum / range problems are reported by setting
// Event::valid=false and Event::error.
std::optional<Event> decode_event(const std::string& line);

// Recompute and verify checksum plus schema/invariant constraints.
bool validate_event(Event& event);

uint64_t compute_checksum(const std::string& prefix);

// Convenience constructors.
Event make_add(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
               Timestamp ts, OrderId order_id, Side side, Price price, Quantity qty);
Event make_cancel(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                  Timestamp ts, OrderId order_id, Quantity qty);
Event make_modify(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                  Timestamp ts, OrderId order_id, Price new_price, Quantity new_qty);
Event make_trade(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                 Timestamp ts, OrderId buy, OrderId sell, Price price, Quantity qty);

} // namespace marketstate

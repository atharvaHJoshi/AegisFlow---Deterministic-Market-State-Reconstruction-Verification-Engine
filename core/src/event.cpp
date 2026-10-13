#include "marketstate/event.hpp"

#include <charconv>
#include <sstream>

namespace marketstate {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

std::string to_str(uint64_t value) { return std::to_string(value); }

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  return tokens;
}

bool parse_u64(const std::string& s, uint64_t& out) {
  auto* begin = s.data();
  auto* end = s.data() + s.size();
  auto res = std::from_chars(begin, end, out);
  return res.ec == std::errc() && res.ptr == end;
}

bool parse_i64(const std::string& s, int64_t& out) {
  auto* begin = s.data();
  auto* end = s.data() + s.size();
  if (begin == end) return false;
  bool neg = false;
  if (*begin == '-') { neg = true; ++begin; }
  if (begin == end) return false;
  uint64_t mag = 0;
  auto res = std::from_chars(begin, end, mag);
  if (res.ec != std::errc() || res.ptr != end) return false;
  out = neg ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
  return true;
}

std::string payload_string(const Event& event) {
  std::ostringstream oss;
  std::visit([&oss](auto&& p) {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, OrderAdd>) {
      oss << p.order_id << ' ' << static_cast<int>(p.side) << ' ' << p.price << ' ' << p.quantity;
    } else if constexpr (std::is_same_v<T, OrderCancel>) {
      oss << p.order_id << ' ' << p.quantity;
    } else if constexpr (std::is_same_v<T, OrderModify>) {
      oss << p.order_id << ' ' << p.new_price << ' ' << p.new_quantity;
    } else if constexpr (std::is_same_v<T, Trade>) {
      oss << p.buy_order_id << ' ' << p.sell_order_id << ' ' << p.price << ' ' << p.quantity;
    } else if constexpr (std::is_same_v<T, MarketUpdate>) {
      oss << p.best_bid << ' ' << p.best_ask;
    }
  }, event.payload);
  return oss.str();
}

int expected_payload_tokens(EventType type) {
  switch (type) {
    case EventType::ORDER_ADD: return 4;
    case EventType::ORDER_CANCEL: return 2;
    case EventType::ORDER_MODIFY: return 3;
    case EventType::TRADE: return 4;
    case EventType::MARKET_UPDATE: return 2;
  }
  return -1;
}

} // namespace

uint64_t compute_checksum(const std::string& prefix) {
  uint64_t hash = kFnvOffset;
  for (unsigned char c : prefix) {
    hash ^= c;
    hash *= kFnvPrime;
  }
  return hash;
}

std::string encode_event(const Event& event) {
  std::ostringstream base;
  base << event.event_id << ' ' << event.stream_id << ' ' << event.sequence_num << ' '
       << event.symbol << ' ' << static_cast<int>(event.type) << ' ' << event.timestamp << ' '
       << payload_string(event);
  std::string base_str = base.str();
  // Checksum trails the record and covers every preceding field, so corruption
  // anywhere (header or payload) is detectable.
  return base_str + " " + to_str(compute_checksum(base_str));
}

std::optional<Event> decode_event(const std::string& line) {
  std::vector<std::string> t = split(line);
  if (t.size() < 8) return std::nullopt;

  Event e;
  uint64_t event_id = 0, stream_id = 0, seq = 0, ts = 0, checksum = 0;
  int64_t type = 0;
  if (!parse_u64(t[0], event_id)) return std::nullopt;
  if (!parse_u64(t[1], stream_id)) return std::nullopt;
  if (!parse_u64(t[2], seq)) return std::nullopt;
  if (!parse_i64(t[4], type)) return std::nullopt;
  if (!parse_u64(t[5], ts)) return std::nullopt;
  if (!parse_u64(t.back(), checksum)) return std::nullopt;

  e.event_id = event_id;
  e.stream_id = static_cast<StreamId>(stream_id);
  e.sequence_num = seq;
  e.symbol = t[3];
  e.type = static_cast<EventType>(type);
  e.timestamp = static_cast<Timestamp>(ts);
  e.checksum = checksum;

  int need = expected_payload_tokens(e.type);
  if (need < 0 || static_cast<int>(t.size()) != 7 + need) {
    e.valid = false;
    e.error = "wrong payload arity for event type";
    return e;
  }
  const size_t checksum_idx = t.size() - 1;

  auto get_i64 = [&](size_t idx, int64_t& out) { return parse_i64(t[idx], out); };
  auto get_u64 = [&](size_t idx, uint64_t& out) { return parse_u64(t[idx], out); };

  int64_t a = 0;
  switch (e.type) {
    case EventType::ORDER_ADD: {
      OrderAdd p;
      uint64_t oid = 0;
      if (!get_u64(6, oid) || !get_i64(7, a) || !get_i64(8, p.price) || !get_i64(9, p.quantity)) {
        e.valid = false; e.error = "bad ORDER_ADD payload"; return e;
      }
      p.order_id = oid;
      p.side = static_cast<Side>(a);
      e.payload = p;
      break;
    }
    case EventType::ORDER_CANCEL: {
      OrderCancel p;
      uint64_t oid = 0;
      if (!get_u64(6, oid) || !get_i64(7, p.quantity)) {
        e.valid = false; e.error = "bad ORDER_CANCEL payload"; return e;
      }
      p.order_id = oid;
      e.payload = p;
      break;
    }
    case EventType::ORDER_MODIFY: {
      OrderModify p;
      uint64_t oid = 0;
      if (!get_u64(6, oid) || !get_i64(7, p.new_price) || !get_i64(8, p.new_quantity)) {
        e.valid = false; e.error = "bad ORDER_MODIFY payload"; return e;
      }
      p.order_id = oid;
      e.payload = p;
      break;
    }
    case EventType::TRADE: {
      Trade p;
      uint64_t buy = 0, sell = 0;
      if (!get_u64(6, buy) || !get_u64(7, sell) || !get_i64(8, p.price) || !get_i64(9, p.quantity)) {
        e.valid = false; e.error = "bad TRADE payload"; return e;
      }
      p.buy_order_id = buy;
      p.sell_order_id = sell;
      e.payload = p;
      break;
    }
    case EventType::MARKET_UPDATE: {
      MarketUpdate p;
      if (!get_i64(6, p.best_bid) || !get_i64(7, p.best_ask)) {
        e.valid = false; e.error = "bad MARKET_UPDATE payload"; return e;
      }
      e.payload = p;
      break;
    }
    default:
      e.valid = false;
      e.error = "unknown event type";
      return e;
  }

  std::string covered;
  for (size_t i = 0; i < checksum_idx; ++i) {
    if (i > 0) covered += ' ';
    covered += t[i];
  }
  if (compute_checksum(covered) != checksum) {
    e.valid = false;
    e.error = "checksum mismatch";
    return e;
  }

  validate_event(e);
  return e;
}

bool validate_event(Event& event) {
  event.valid = true;
  event.error.clear();

  auto fail = [&](const std::string& msg) {
    event.valid = false;
    event.error = msg;
    return false;
  };

  if (event.symbol.empty()) return fail("empty symbol");
  if (event.sequence_num == 0) return fail("non-positive sequence");
  if (static_cast<int>(event.type) < 0 || static_cast<int>(event.type) > 4) {
    return fail("invalid event type");
  }

  bool ok = std::visit([&](auto&& p) -> bool {
    using T = std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T, OrderAdd>) {
      if (p.side != Side::BID && p.side != Side::ASK) return fail("invalid side");
      if (p.price < 0) return fail("negative price");
      if (p.quantity <= 0) return fail("non-positive quantity");
    } else if constexpr (std::is_same_v<T, OrderCancel>) {
      if (p.quantity != kNoQuantity && p.quantity < 0) return fail("invalid cancel quantity");
    } else if constexpr (std::is_same_v<T, OrderModify>) {
      if (p.new_price != kNoPrice && p.new_price < 0) return fail("invalid modify price");
      if (p.new_quantity != kNoQuantity && p.new_quantity < 0) return fail("invalid modify quantity");
    } else if constexpr (std::is_same_v<T, Trade>) {
      if (p.price < 0) return fail("negative trade price");
      if (p.quantity <= 0) return fail("non-positive trade quantity");
    } else if constexpr (std::is_same_v<T, MarketUpdate>) {
      if (p.best_bid != kNoPrice && p.best_bid < 0) return fail("invalid market bid");
      if (p.best_ask != kNoPrice && p.best_ask < 0) return fail("invalid market ask");
    }
    return true;
  }, event.payload);

  if (!ok) return false;

  // Verify checksum against the re-encoded record (header + payload).
  std::ostringstream base;
  base << event.event_id << ' ' << event.stream_id << ' ' << event.sequence_num << ' '
       << event.symbol << ' ' << static_cast<int>(event.type) << ' ' << event.timestamp << ' '
       << payload_string(event);
  if (compute_checksum(base.str()) != event.checksum) return fail("checksum mismatch");

  return true;
}

namespace {

Event make_base(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                Timestamp ts, EventType type) {
  Event e;
  e.event_id = id;
  e.stream_id = stream;
  e.sequence_num = seq;
  e.symbol = symbol;
  e.timestamp = ts;
  e.type = type;
  return e;
}

void finalize(Event& e) {
  std::ostringstream base;
  base << e.event_id << ' ' << e.stream_id << ' ' << e.sequence_num << ' '
       << e.symbol << ' ' << static_cast<int>(e.type) << ' ' << e.timestamp << ' '
       << payload_string(e);
  e.checksum = compute_checksum(base.str());
  e.valid = true;
}

} // namespace

Event make_add(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
               Timestamp ts, OrderId order_id, Side side, Price price, Quantity qty) {
  Event e = make_base(id, stream, seq, symbol, ts, EventType::ORDER_ADD);
  e.payload = OrderAdd{order_id, side, price, qty};
  finalize(e);
  return e;
}

Event make_cancel(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                  Timestamp ts, OrderId order_id, Quantity qty) {
  Event e = make_base(id, stream, seq, symbol, ts, EventType::ORDER_CANCEL);
  e.payload = OrderCancel{order_id, qty};
  finalize(e);
  return e;
}

Event make_modify(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                  Timestamp ts, OrderId order_id, Price new_price, Quantity new_qty) {
  Event e = make_base(id, stream, seq, symbol, ts, EventType::ORDER_MODIFY);
  e.payload = OrderModify{order_id, new_price, new_qty};
  finalize(e);
  return e;
}

Event make_trade(EventId id, StreamId stream, SeqNum seq, const std::string& symbol,
                 Timestamp ts, OrderId buy, OrderId sell, Price price, Quantity qty) {
  Event e = make_base(id, stream, seq, symbol, ts, EventType::TRADE);
  e.payload = Trade{buy, sell, price, qty};
  finalize(e);
  return e;
}

} // namespace marketstate

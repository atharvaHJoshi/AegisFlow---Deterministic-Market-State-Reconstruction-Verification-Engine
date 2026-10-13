#pragma once

#include "marketstate/event.hpp"

#include <string>
#include <vector>

namespace testutil {

using namespace marketstate;

inline Event add(uint64_t id, uint64_t seq, const std::string& sym, Side side, Price price,
                 Quantity qty, StreamId stream = 1) {
  return make_add(id, stream, seq, sym, static_cast<Timestamp>(seq * 1000), id, side, price, qty);
}

inline Event cancel(uint64_t id, uint64_t seq, const std::string& sym, uint64_t order_id,
                    Quantity qty = kNoQuantity, StreamId stream = 1) {
  return make_cancel(id, stream, seq, sym, static_cast<Timestamp>(seq * 1000), order_id, qty);
}

inline Event modify(uint64_t id, uint64_t seq, const std::string& sym, uint64_t order_id,
                    Price new_price, Quantity new_qty, StreamId stream = 1) {
  return make_modify(id, stream, seq, sym, static_cast<Timestamp>(seq * 1000), order_id, new_price,
                     new_qty);
}

inline Event trade(uint64_t id, uint64_t seq, const std::string& sym, uint64_t buy, uint64_t sell,
                   Price price, Quantity qty, StreamId stream = 1) {
  return make_trade(id, stream, seq, sym, static_cast<Timestamp>(seq * 1000), buy, sell, price, qty);
}

} // namespace testutil

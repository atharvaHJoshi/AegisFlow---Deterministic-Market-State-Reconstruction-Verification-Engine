#include "marketstate/order_book.hpp"

#include <algorithm>
#include <sstream>
#include <vector>

namespace marketstate {

OrderBook::OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

bool OrderBook::apply_event(const Event& event) {
  if (!event.valid) return false;
  if (!symbol_.empty() && event.symbol != symbol_) return false;

  std::visit([&](auto&& data) {
    using T = std::decay_t<decltype(data)>;
    if constexpr (std::is_same_v<T, OrderAdd>) {
      add_order(event, data);
    } else if constexpr (std::is_same_v<T, OrderCancel>) {
      cancel_order(event, data);
    } else if constexpr (std::is_same_v<T, OrderModify>) {
      modify_order(event, data);
    } else if constexpr (std::is_same_v<T, Trade>) {
      process_trade(event, data);
    } else if constexpr (std::is_same_v<T, MarketUpdate>) {
      // Market updates are advisory metadata; the book is authoritative.
    }
  }, event.payload);

  last_applied_seq_ = std::max(last_applied_seq_, event.sequence_num);
  return true;
}

void OrderBook::level_add(Side side, Price price, Quantity qty) {
  if (qty <= 0) return;
  if (side == Side::BID) {
    auto& level = bids_[price];
    level.price = price;
    level.total_quantity += qty;
  } else {
    auto& level = asks_[price];
    level.price = price;
    level.total_quantity += qty;
  }
}

void OrderBook::level_remove(Side side, Price price, Quantity qty) {
  if (qty <= 0) return;
  if (side == Side::BID) {
    auto it = bids_.find(price);
    if (it == bids_.end()) return;
    it->second.total_quantity -= qty;
    if (it->second.total_quantity <= 0) bids_.erase(it);
  } else {
    auto it = asks_.find(price);
    if (it == asks_.end()) return;
    it->second.total_quantity -= qty;
    if (it->second.total_quantity <= 0) asks_.erase(it);
  }
}

void OrderBook::add_order(const Event& event, const OrderAdd& add) {
  auto it = orders_.find(add.order_id);
  if (it != orders_.end() && it->second.active) {
    // Duplicate logical add: deterministic no-op rather than corrupting state.
    return;
  }
  Order order;
  order.order_id = add.order_id;
  order.side = add.side;
  order.price = add.price;
  order.quantity = add.quantity;
  order.last_seq = event.sequence_num;
  order.active = true;
  orders_[add.order_id] = order;
  level_add(add.side, add.price, add.quantity);
}

void OrderBook::cancel_order(const Event& event, const OrderCancel& cancel) {
  auto it = orders_.find(cancel.order_id);
  if (it == orders_.end() || !it->second.active) return;
  Order& order = it->second;

  Quantity remove = (cancel.quantity == kNoQuantity)
                        ? order.quantity
                        : std::min(cancel.quantity, order.quantity);
  if (remove <= 0) return;

  level_remove(order.side, order.price, remove);
  order.quantity -= remove;
  order.last_seq = event.sequence_num;
  if (order.quantity == 0) order.active = false;
}

void OrderBook::modify_order(const Event& event, const OrderModify& modify) {
  auto it = orders_.find(modify.order_id);
  if (it == orders_.end() || !it->second.active) return;
  Order& order = it->second;

  Price new_price = (modify.new_price == kNoPrice) ? order.price : modify.new_price;
  Quantity new_qty = (modify.new_quantity == kNoQuantity) ? order.quantity : modify.new_quantity;

  level_remove(order.side, order.price, order.quantity);
  order.price = new_price;
  order.quantity = new_qty;
  order.last_seq = event.sequence_num;
  if (order.quantity <= 0) {
    order.quantity = 0;
    order.active = false;
  } else {
    level_add(order.side, order.price, order.quantity);
  }
}

void OrderBook::process_trade(const Event&, const Trade& trade) {
  total_volume_ += trade.quantity;
  total_trades_ += 1;
}

BookSnapshot OrderBook::snapshot() const {
  return BookSnapshot{symbol_, best_bid(), best_ask(), total_volume_, total_trades_,
                      last_applied_seq_};
}

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
  auto bb = best_bid();
  auto ba = best_ask();
  if (!bb || !ba) return std::nullopt;
  return *ba - *bb;
}

std::optional<double> OrderBook::midpoint() const {
  auto bb = best_bid();
  auto ba = best_ask();
  if (!bb || !ba) return std::nullopt;
  return (static_cast<double>(*bb) + static_cast<double>(*ba)) / 2.0;
}

const Order* OrderBook::find_order(OrderId id) const {
  auto it = orders_.find(id);
  if (it == orders_.end()) return nullptr;
  return &it->second;
}

std::string OrderBook::canonical_state() const {
  std::vector<const Order*> active;
  active.reserve(orders_.size());
  for (const auto& [id, order] : orders_) {
    if (order.active && order.quantity > 0) active.push_back(&order);
  }
  std::sort(active.begin(), active.end(),
            [](const Order* a, const Order* b) { return a->order_id < b->order_id; });

  std::ostringstream oss;
  oss << "STATE " << symbol_ << '\n';
  oss << "SEQ " << last_applied_seq_ << '\n';
  oss << "VOL " << total_volume_ << '\n';
  oss << "TRD " << total_trades_ << '\n';
  oss << "LEVELS " << bids_.size() << ' ' << asks_.size() << '\n';
  for (const Order* o : active) {
    oss << "O " << o->order_id << ' ' << static_cast<int>(o->side) << ' ' << o->price << ' '
        << o->quantity << '\n';
  }
  oss << "END";
  return oss.str();
}

bool OrderBook::load_canonical_state(const std::string& state) {
  clear();
  std::istringstream iss(state);
  std::string line;
  bool saw_end = false;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    if (line.rfind("STATE ", 0) == 0) {
      symbol_ = line.substr(6);
    } else if (line.rfind("SEQ ", 0) == 0) {
      last_applied_seq_ = static_cast<SeqNum>(std::stoll(line.substr(4)));
    } else if (line.rfind("VOL ", 0) == 0) {
      total_volume_ = std::stoll(line.substr(4));
    } else if (line.rfind("TRD ", 0) == 0) {
      total_trades_ = std::stoll(line.substr(4));
    } else if (line.rfind("LEVELS ", 0) == 0) {
      // Derived from orders; ignored on load.
    } else if (line.rfind("O ", 0) == 0) {
      std::istringstream ls(line.substr(2));
      long long id = 0, side = 0, price = 0, qty = 0;
      if (!(ls >> id >> side >> price >> qty)) return false;
      if (qty <= 0) continue;
      Order order;
      order.order_id = static_cast<OrderId>(id);
      order.side = static_cast<Side>(side);
      order.price = static_cast<Price>(price);
      order.quantity = static_cast<Quantity>(qty);
      order.active = true;
      orders_[order.order_id] = order;
      level_add(order.side, order.price, order.quantity);
    } else if (line == "END") {
      saw_end = true;
    } else {
      return false;
    }
  }
  return saw_end;
}

void OrderBook::clear() {
  orders_.clear();
  bids_.clear();
  asks_.clear();
  total_volume_ = 0;
  total_trades_ = 0;
  last_applied_seq_ = 0;
}

} // namespace marketstate

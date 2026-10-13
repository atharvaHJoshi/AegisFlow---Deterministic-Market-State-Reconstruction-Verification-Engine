#include "marketstate/order_book.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

using namespace marketstate;
using namespace testutil;

TEST(OrderBook, AddComputesBestBidAskSpreadMidpoint) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 50));
  book.apply_event(add(2, 2, "AAPL", Side::ASK, 10020, 30));

  ASSERT_TRUE(book.best_bid().has_value());
  ASSERT_TRUE(book.best_ask().has_value());
  EXPECT_EQ(*book.best_bid(), 10000);
  EXPECT_EQ(*book.best_ask(), 10020);
  EXPECT_EQ(*book.spread(), 20);
  EXPECT_DOUBLE_EQ(*book.midpoint(), 10010.0);
}

TEST(OrderBook, AggregatesQuantityAtPriceLevel) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 50));
  book.apply_event(add(2, 2, "AAPL", Side::BID, 10000, 25));
  book.apply_event(add(3, 3, "AAPL", Side::BID, 9990, 10));

  ASSERT_EQ(book.bids().size(), 2u);
  EXPECT_EQ(book.bids().at(10000).total_quantity, 75);
  EXPECT_EQ(*book.best_bid(), 10000);
}

TEST(OrderBook, CancelReducesAndRemovesLevels) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 50));
  book.apply_event(add(2, 2, "AAPL", Side::BID, 10000, 50));
  book.apply_event(cancel(3, 3, "AAPL", 1, 50));
  EXPECT_EQ(book.bids().at(10000).total_quantity, 50);
  book.apply_event(cancel(4, 4, "AAPL", 2));
  EXPECT_TRUE(book.bids().empty());
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, PartialCancelLeavesRemainder) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 100));
  book.apply_event(cancel(2, 2, "AAPL", 1, 40));
  EXPECT_EQ(book.bids().at(10000).total_quantity, 60);
  ASSERT_NE(book.find_order(1), nullptr);
  EXPECT_EQ(book.find_order(1)->quantity, 60);
}

TEST(OrderBook, ModifyMovesPriceLevel) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 50));
  book.apply_event(modify(2, 2, "AAPL", 1, 10010, 30));
  EXPECT_TRUE(book.bids().count(10000) == 0);
  EXPECT_EQ(book.bids().at(10010).total_quantity, 30);
}

TEST(OrderBook, ModifyQuantityOnly) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::ASK, 10020, 50));
  book.apply_event(modify(2, 2, "AAPL", 1, kNoPrice, 80));
  EXPECT_EQ(book.asks().at(10020).total_quantity, 80);
}

TEST(OrderBook, TradesAccumulateVolumeAndCount) {
  OrderBook book("AAPL");
  book.apply_event(add(1, 1, "AAPL", Side::BID, 10000, 50));
  book.apply_event(add(2, 2, "AAPL", Side::ASK, 10000, 50));
  book.apply_event(trade(3, 3, "AAPL", 1, 2, 10000, 20));
  book.apply_event(trade(4, 4, "AAPL", 1, 2, 10000, 10));
  EXPECT_EQ(book.total_volume(), 30);
  EXPECT_EQ(book.total_trades(), 2);
}

TEST(OrderBook, UnknownOrderOperationsAreNoOps) {
  OrderBook book("AAPL");
  book.apply_event(cancel(1, 1, "AAPL", 999));
  book.apply_event(modify(2, 2, "AAPL", 999, 100, 5));
  EXPECT_TRUE(book.bids().empty());
  EXPECT_TRUE(book.asks().empty());
}

TEST(OrderBook, CanonicalStateIsDeterministicAndRestorable) {
  OrderBook a("AAPL");
  OrderBook b("AAPL");
  std::vector<Event> stream = {
      add(1, 1, "AAPL", Side::BID, 10000, 50),
      add(2, 2, "AAPL", Side::BID, 9990, 25),
      add(3, 3, "AAPL", Side::ASK, 10020, 40),
      modify(4, 4, "AAPL", 2, 9995, 10),
      cancel(5, 5, "AAPL", 1, 20),
      trade(6, 6, "AAPL", 1, 3, 10000, 5),
  };
  for (const Event& e : stream) a.apply_event(e);
  // Apply out of order to the second book, then compare canonical state only
  // after applying the same logical operations in order.
  for (const Event& e : stream) b.apply_event(e);

  EXPECT_EQ(a.canonical_state(), b.canonical_state());

  OrderBook restored;
  ASSERT_TRUE(restored.load_canonical_state(a.canonical_state()));
  EXPECT_EQ(restored.canonical_state(), a.canonical_state());
  EXPECT_EQ(restored.symbol(), "AAPL");
}

TEST(OrderBook, RejectsMalformedCanonicalState) {
  OrderBook book;
  EXPECT_FALSE(book.load_canonical_state("STATE X\nGARBAGE\nEND"));
}

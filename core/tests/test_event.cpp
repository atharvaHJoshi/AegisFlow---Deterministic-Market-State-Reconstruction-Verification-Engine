#include "marketstate/event.hpp"
#include "test_helpers.hpp"

#include <gtest/gtest.h>

using namespace marketstate;
using namespace testutil;

TEST(EventCodec, RoundTripsEveryType) {
  std::vector<Event> events = {
      add(1, 1, "AAPL", Side::BID, 10000, 50),
      cancel(2, 2, "AAPL", 1, kNoQuantity),
      cancel(3, 3, "AAPL", 1, 20),
      modify(4, 4, "AAPL", 1, 10050, 30),
      modify(5, 5, "AAPL", 1, kNoPrice, 40),
      trade(6, 6, "AAPL", 1, 2, 10000, 10),
  };
  for (const Event& e : events) {
    std::string encoded = encode_event(e);
    auto decoded = decode_event(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->valid) << decoded->error;
    EXPECT_EQ(encode_event(*decoded), encoded);
  }
}

TEST(EventCodec, MarketUpdateRoundTrips) {
  Event e;
  e.event_id = 9;
  e.stream_id = 1;
  e.sequence_num = 9;
  e.symbol = "MSFT";
  e.type = EventType::MARKET_UPDATE;
  e.timestamp = 9000;
  e.payload = MarketUpdate{9950, 10050};
  std::string encoded = encode_event(e);
  auto decoded = decode_event(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->valid);
  EXPECT_EQ(encode_event(*decoded), encoded);
}

TEST(EventCodec, ChecksumDetectsCorruption) {
  Event e = add(1, 1, "AAPL", Side::BID, 10000, 50);
  std::string encoded = encode_event(e);
  // Corrupt a payload byte without fixing the checksum.
  encoded[encoded.size() - 1] = (encoded.back() == '9') ? '8' : '9';
  auto decoded = decode_event(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->valid);
  EXPECT_EQ(decoded->error, "checksum mismatch");
}

TEST(EventValidation, RejectsImpossibleValues) {
  Event e = add(1, 1, "AAPL", Side::BID, 10000, 50);
  // Re-encode with a negative quantity by hacking the payload then recomputing.
  std::get<OrderAdd>(e.payload).quantity = -5;
  EXPECT_FALSE(validate_event(e));
  EXPECT_EQ(e.error, "non-positive quantity");
}

TEST(EventValidation, RejectsEmptySymbol) {
  Event e = add(1, 1, "", Side::BID, 10000, 50);
  EXPECT_FALSE(validate_event(e));
}

TEST(EventCodec, RejectsWrongPayloadArity) {
  // ORDER_ADD needs 4 payload fields + trailing checksum; this line has too few.
  auto decoded = decode_event("1 1 1 AAPL 0 1000 5 5");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->valid);
}

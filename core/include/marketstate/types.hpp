#pragma once

#include <cstdint>
#include <string>

namespace marketstate {

// Determinism note: all prices are integer ticks and all quantities are
// integer units. Float is deliberately avoided everywhere in market state so
// that replay, checkpoint/recovery and the independent reference model are
// bit-for-bit comparable.
using Price = int64_t;
using Quantity = int64_t;
using EventId = uint64_t;
using OrderId = uint64_t;
using SeqNum = uint64_t;
using StreamId = uint32_t;
using Timestamp = int64_t; // nanoseconds since epoch, supplied by the stream

constexpr Price kNoPrice = -1;
constexpr Quantity kNoQuantity = -1;
constexpr SeqNum kNoSeq = 0;

} // namespace marketstate

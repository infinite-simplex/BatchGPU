#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "BatchCommand.hpp"
#include "Limits.hpp"

// The client's frame on the TCP stream:
//
//     [2 bytes: payload length][payload]
//
// The payload is a type byte and then a body of fixed size for that type, and the length counts
// both. Every integer is big endian. The three messages:
//
//   'O'  ORDER (73 bytes)                       'X'  CANCEL (13 bytes)    'C'  BATCH_CLEAR (9 bytes)
//     offset  size  field                         1   4  participantId     1   8  batchId
//     0       1     'O'                           5   8  orderToken
//     1       4     participantId
//     5       8     orderToken
//     13      1     side: 'B' bid, 'A' ask
//     14      4     acceleratorMask
//     18      1     interconnectMask
//     19      4     regionMask
//     23      4     baseHour
//     27      8     slotMask
//     35      4     perSlotCap
//     39      1     workloadClass (reserved, ignored)
//     40      1     stepCount
//     41      32    four steps of { quantity u32, price u32 }; only the first stepCount count
//
// An ORDER always carries all four step slots, so its size is fixed.
namespace wire {

inline constexpr std::size_t LENGTH_FIELD_SIZE = 2;

inline constexpr char ORDER = 'O';
inline constexpr char CANCEL = 'X';
inline constexpr char BATCH_CLEAR = 'C';

inline constexpr std::size_t ORDER_PAYLOAD_SIZE = 41 + 8 * limits::MAX_STEPS;
inline constexpr std::size_t CANCEL_PAYLOAD_SIZE = 13;
inline constexpr std::size_t BATCH_CLEAR_PAYLOAD_SIZE = 9;

// No valid frame is longer, so a declared length over this is refused before the handler waits for
// the rest of the frame: a client cannot make the gateway buffer a long one.
inline constexpr std::size_t MAX_PAYLOAD_SIZE = ORDER_PAYLOAD_SIZE;

static_assert(CANCEL_PAYLOAD_SIZE <= MAX_PAYLOAD_SIZE && BATCH_CLEAR_PAYLOAD_SIZE <= MAX_PAYLOAD_SIZE,
              "MAX_PAYLOAD_SIZE must be the largest payload");
static_assert(MAX_PAYLOAD_SIZE <= 0xFFFF, "the length field is two bytes");

// Encoding, for clients and tests. The handler does not use it.
inline void putU8(std::vector<char>& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
inline void putU16(std::vector<char>& out, std::uint16_t v) {
    putU8(out, static_cast<std::uint8_t>(v >> 8));
    putU8(out, static_cast<std::uint8_t>(v));
}
inline void putU32(std::vector<char>& out, std::uint32_t v) {
    putU16(out, static_cast<std::uint16_t>(v >> 16));
    putU16(out, static_cast<std::uint16_t>(v));
}
inline void putU64(std::vector<char>& out, std::uint64_t v) {
    putU32(out, static_cast<std::uint32_t>(v >> 32));
    putU32(out, static_cast<std::uint32_t>(v));
}

// Sends all four step slots as they are in `c`, so a test can put anything past stepCount, and
// `workloadClass` sets the reserved byte.
inline std::vector<char> encodeOrder(const BatchCommand& c, std::uint8_t workloadClass = 0) {
    std::vector<char> out;
    out.reserve(LENGTH_FIELD_SIZE + ORDER_PAYLOAD_SIZE);
    putU16(out, static_cast<std::uint16_t>(ORDER_PAYLOAD_SIZE));
    putU8(out, static_cast<std::uint8_t>(ORDER));
    putU32(out, c.participantId);
    putU64(out, c.orderToken);
    putU8(out, static_cast<std::uint8_t>(c.side));
    putU32(out, c.acceleratorMask);
    putU8(out, c.interconnectMask);
    putU32(out, c.regionMask);
    putU32(out, c.baseHour);
    putU64(out, c.slotMask);
    putU32(out, c.perSlotCap);
    putU8(out, workloadClass);
    putU8(out, c.stepCount);
    for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) {
        putU32(out, c.steps[k].quantity);
        putU32(out, c.steps[k].price);
    }
    return out;
}

inline std::vector<char> encodeCancel(ParticipantId participant, OrderToken token) {
    std::vector<char> out;
    putU16(out, static_cast<std::uint16_t>(CANCEL_PAYLOAD_SIZE));
    putU8(out, static_cast<std::uint8_t>(CANCEL));
    putU32(out, participant);
    putU64(out, token);
    return out;
}

inline std::vector<char> encodeBatchClear(BatchId id) {
    std::vector<char> out;
    putU16(out, static_cast<std::uint16_t>(BATCH_CLEAR_PAYLOAD_SIZE));
    putU8(out, static_cast<std::uint8_t>(BATCH_CLEAR));
    putU64(out, id);
    return out;
}

} // namespace wire

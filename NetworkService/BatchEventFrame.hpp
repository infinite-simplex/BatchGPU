#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>

#include "BatchEvent.hpp"
#include "BatchWire.hpp"
#include "RejectReason.hpp"

// The frame that clients receive, privately for their own orders and publicly for everyone's:
//
//     [2 bytes: payload length][payload]
//
// A public datagram holds as many whole frames as fit in the gateway's egressDatagramMaxBytes, back to back
// (Core v0.3.0 and later), so a reader of the public feed finds the boundaries from each frame's length field.
// A private stream is a TCP stream of the same frames.
//
// The payload is a version byte (PROTOCOL_VERSION, see BatchWire.hpp), a type byte and then the same body
// for every type, so a frame is always EVENT_FRAME_SIZE bytes. A field means something only for the types
// that BatchEvent.hpp lists it under, and is zero otherwise. Every integer is big endian, and the session
// id, which only routes, is not sent.
//
//   type: 'A' ACCEPTED, 'J' REJECTED, 'C' CANCELED, 'F' FILL, 'D' ORDER_DONE, 'B' BATCH_DONE, 'V' BATCH_VOIDED
//
//   offset  size  field             offset  size  field
//   0       1     version           42      4     hour
//   1       1     type              46      4     price
//   2       8     sequence number   50      4     units
//   10      8     batch id          54      4     unfilled units
//   18      4     participant id    58      4     lifted units
//   22      8     order token       62      8     amount (signed)
//   30      1     side              70      8     rank sum (signed)
//   31      4     rank              78      4     fills
//   35      1     reject reason     82      4     bids
//   36      2     reject detail     86      4     asks
//   38      1     accelerator       90      4     arcs
//   39      1     interconnect      94      4     pivots
//   40      1     region            98      1     certified
//   41      1     lifted            99      1     void reason
namespace wire {

inline constexpr std::size_t EVENT_PAYLOAD_SIZE = 100;
inline constexpr std::size_t EVENT_FRAME_SIZE = LENGTH_FIELD_SIZE + EVENT_PAYLOAD_SIZE;

inline constexpr EventType EVENT_TYPES[] = { EventType::ACCEPTED,   EventType::REJECTED,   EventType::CANCELED,
                                             EventType::FILL,       EventType::ORDER_DONE, EventType::BATCH_DONE,
                                             EventType::BATCH_VOIDED };

constexpr char eventTypeByte(EventType type) {
    switch (type) {
    case EventType::ACCEPTED: return 'A';
    case EventType::REJECTED: return 'J';
    case EventType::CANCELED: return 'C';
    case EventType::FILL: return 'F';
    case EventType::ORDER_DONE: return 'D';
    case EventType::BATCH_DONE: return 'B';
    case EventType::BATCH_VOIDED: return 'V';
    }
    return '?';
}

inline std::vector<char> encodeEvent(const BatchEvent& e) {
    std::vector<char> out;
    out.reserve(EVENT_FRAME_SIZE);
    putU16(out, static_cast<std::uint16_t>(EVENT_PAYLOAD_SIZE));
    putU8(out, PROTOCOL_VERSION);
    putU8(out, static_cast<std::uint8_t>(eventTypeByte(e.type)));
    putU64(out, e.sequence_number);
    putU64(out, e.batchId);
    putU32(out, e.participantId);
    putU64(out, e.orderToken);
    putU8(out, static_cast<std::uint8_t>(e.side));
    putU32(out, e.rank);
    putU8(out, static_cast<std::uint8_t>(e.rejectReason));
    putU16(out, e.rejectDetail);
    putU8(out, e.accelerator);
    putU8(out, e.interconnect);
    putU8(out, e.region);
    putU8(out, e.lifted);
    putU32(out, e.hour);
    putU32(out, e.price);
    putU32(out, e.units);
    putU32(out, e.unfilledUnits);
    putU32(out, e.liftedUnits);
    putU64(out, static_cast<std::uint64_t>(e.amount));
    putU64(out, static_cast<std::uint64_t>(e.rankSum));
    putU32(out, e.fills);
    putU32(out, e.bids);
    putU32(out, e.asks);
    putU32(out, e.arcs);
    putU32(out, e.pivots);
    putU8(out, e.certified);
    putU8(out, static_cast<std::uint8_t>(e.voidReason));
    return out;
}

// One whole frame, for clients and tests. It says nothing of a value it does not know (a newer reject reason,
// say), only of a frame that is not one: the wrong size, length, version or type.
inline std::optional<BatchEvent> decodeEvent(const char* data, std::size_t size) {
    if (size != EVENT_FRAME_SIZE) return std::nullopt;
    const auto* at = reinterpret_cast<const std::uint8_t*>(data);
    auto take = [&at](std::size_t bytes) {
        std::uint64_t value = 0;
        for (; bytes > 0; --bytes) value = (value << 8) | *at++;
        return value;
    };
    if (take(2) != EVENT_PAYLOAD_SIZE) return std::nullopt;
    if (take(1) != PROTOCOL_VERSION) return std::nullopt;

    const char typeByte = static_cast<char>(take(1));
    BatchEvent e{};
    bool known = false;
    for (const EventType type : EVENT_TYPES) {
        if (eventTypeByte(type) == typeByte) {
            e.type = type;
            known = true;
        }
    }
    if (!known) return std::nullopt;

    e.sequence_number = take(8);
    e.batchId = take(8);
    e.participantId = static_cast<ParticipantId>(take(4));
    e.orderToken = take(8);
    e.side = static_cast<Side>(take(1));
    e.rank = static_cast<std::uint32_t>(take(4));
    e.rejectReason = static_cast<RejectReason>(take(1));
    e.rejectDetail = static_cast<std::uint16_t>(take(2));
    e.accelerator = static_cast<std::uint8_t>(take(1));
    e.interconnect = static_cast<std::uint8_t>(take(1));
    e.region = static_cast<std::uint8_t>(take(1));
    e.lifted = static_cast<std::uint8_t>(take(1));
    e.hour = static_cast<HourIndex>(take(4));
    e.price = static_cast<PriceTicks>(take(4));
    e.units = static_cast<NodeUnits>(take(4));
    e.unfilledUnits = static_cast<NodeUnits>(take(4));
    e.liftedUnits = static_cast<NodeUnits>(take(4));
    e.amount = static_cast<std::int64_t>(take(8));
    e.rankSum = static_cast<std::int64_t>(take(8));
    e.fills = static_cast<std::uint32_t>(take(4));
    e.bids = static_cast<std::uint32_t>(take(4));
    e.asks = static_cast<std::uint32_t>(take(4));
    e.arcs = static_cast<std::uint32_t>(take(4));
    e.pivots = static_cast<std::uint32_t>(take(4));
    e.certified = static_cast<std::uint8_t>(take(1));
    e.voidReason = static_cast<VoidReason>(take(1));
    return e;
}

} // namespace wire

// What Core's EgressGateway asks of an encoder: the bytes to send, and a line for the console.
struct BatchEventEncoder {
    static std::vector<char> encode(const BatchEvent& e) { return wire::encodeEvent(e); }

    static void print(std::ostream& os, const BatchEvent& e) {
        os << toString(e.type) << " seq=" << e.sequence_number << " batch=" << e.batchId << " session=" << e.session_id;
        if (e.type == EventType::BATCH_DONE || e.type == EventType::BATCH_VOIDED) {
            os << " first_hour=" << e.hour << " bids=" << e.bids << " asks=" << e.asks << " arcs=" << e.arcs
               << " pivots=" << e.pivots;
            if (e.type == EventType::BATCH_VOIDED) {
                os << " void_reason=" << toString(e.voidReason);
            } else {
                os << " units=" << e.units << " lifted=" << e.liftedUnits << " welfare=" << e.amount
                   << " rank_sum=" << e.rankSum << " fills=" << e.fills << " certified=" << static_cast<int>(e.certified);
            }
            return;
        }
        const char side = e.side == Side::BID ? 'B' : e.side == Side::ASK ? 'A' : '-';
        os << " who=" << e.participantId << "/" << e.orderToken << " side=" << side;
        switch (e.type) {
        case EventType::ACCEPTED:
        case EventType::CANCELED:
            os << " rank=" << e.rank << " units=" << e.units;
            break;
        case EventType::REJECTED:
            os << " reason=" << toString(e.rejectReason) << " detail=" << e.rejectDetail;
            if (e.rejectReason == RejectReason::SLOT_NOT_ELIGIBLE) os << " hour=" << e.hour;
            break;
        case EventType::FILL:
            os << " rank=" << e.rank << " item=" << static_cast<int>(e.accelerator) << "/"
               << static_cast<int>(e.interconnect) << "/" << static_cast<int>(e.region) << "@" << e.hour
               << " price=" << e.price << " units=" << e.units << " lifted=" << static_cast<int>(e.lifted);
            break;
        case EventType::ORDER_DONE:
            os << " rank=" << e.rank << " units=" << e.units << " unfilled=" << e.unfilledUnits
               << " lifted=" << e.liftedUnits << " amount=" << e.amount;
            break;
        default:
            break;
        }
    }
};

#include "BatchProtocolHandler.hpp"

#include <cstdint>

#include "BatchWire.hpp"
#include "OrderChecks.hpp"

namespace {

// The caller has already checked that the whole frame is in the buffer, so a read never runs past it.
class Reader {
public:
    explicit Reader(const char* p) : p_(reinterpret_cast<const unsigned char*>(p)) {}

    std::uint8_t u8() { return *p_++; }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | *p_++;
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | *p_++;
        return v;
    }

private:
    const unsigned char* p_;
};

std::uint16_t readLength(const char* buf) {
    return static_cast<std::uint16_t>((static_cast<unsigned char>(buf[0]) << 8) | static_cast<unsigned char>(buf[1]));
}

} // namespace

std::size_t BatchProtocolHandler::validateAndParse(const char* buf, std::size_t availableLen,
                                                   BatchCommand& outCmd) const {
    using namespace wire;

    if (availableLen < LENGTH_FIELD_SIZE) return 0;

    const std::size_t payloadLen = readLength(buf);
    // Refuse now, without waiting for bytes that will never make it parse.
    if (payloadLen < 2 || payloadLen > MAX_PAYLOAD_SIZE) return PARSE_FATAL_ERROR;

    // The version says how to read the rest, so a frame of another version is refused as soon as it shows.
    if (availableLen > LENGTH_FIELD_SIZE && static_cast<std::uint8_t>(buf[LENGTH_FIELD_SIZE]) != PROTOCOL_VERSION) {
        return PARSE_FATAL_ERROR;
    }

    const std::size_t frameLen = LENGTH_FIELD_SIZE + payloadLen;
    if (availableLen < frameLen) return 0;

    Reader in(buf + LENGTH_FIELD_SIZE);
    in.u8(); // the version, checked above
    const char type = static_cast<char>(in.u8());
    outCmd = BatchCommand{}; // the gateway writes trace_id and sessionId after this returns

    switch (type) {
    case ORDER: {
        if (payloadLen != ORDER_PAYLOAD_SIZE) return PARSE_FATAL_ERROR;
        outCmd.type = CommandType::ORDER;
        outCmd.participantId = in.u32();
        outCmd.orderToken = in.u64();
        outCmd.side = static_cast<Side>(in.u8());
        outCmd.acceleratorMask = in.u32();
        outCmd.interconnectMask = in.u8();
        outCmd.regionMask = in.u32();
        outCmd.baseHour = in.u32();
        outCmd.slotMask = in.u64();
        outCmd.perSlotCap = in.u32();
        in.u8(); // the workload class: reserved, read and ignored
        outCmd.stepCount = in.u8();
        for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) {
            outCmd.steps[k].quantity = in.u32();
            outCmd.steps[k].price = in.u32();
        }

        // A well-formed frame becomes a command even when the order is invalid: an INVALID command,
        // which the engine refuses with an event.
        const Reject reject = checkOrder(outCmd);
        if (reject) {
            outCmd.type = CommandType::INVALID;
            outCmd.invalidReason = reject.reason;
            outCmd.invalidDetail = reject.detail;
        }
        return frameLen;
    }

    case CANCEL: {
        if (payloadLen != CANCEL_PAYLOAD_SIZE) return PARSE_FATAL_ERROR;
        outCmd.type = CommandType::CANCEL;
        outCmd.participantId = in.u32();
        outCmd.orderToken = in.u64();
        // A token that names no live order is not a protocol error here: the engine refuses it
        // (UNKNOWN_ORDER), because only the engine knows which orders are live.
        return frameLen;
    }

    case BATCH_CLEAR: {
        if (payloadLen != BATCH_CLEAR_PAYLOAD_SIZE) return PARSE_FATAL_ERROR;
        outCmd.type = CommandType::BATCH_CLEAR;
        outCmd.batchId = in.u64(); // any id is accepted
        return frameLen;
    }

    default:
        return PARSE_FATAL_ERROR;
    }
}

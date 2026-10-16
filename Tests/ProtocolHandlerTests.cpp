#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "BatchEvent.hpp"
#include "BatchEventFrame.hpp"
#include "BatchProtocolHandler.hpp"
#include "BatchWire.hpp"
#include "EngineFixtures.hpp"
#include "EgressBatch.hpp"
#include "NetworkGateway.hpp"
#include "OrderChecks.hpp"
#include "OrderFixtures.hpp"
#include "PageEdge.hpp"
#include "SequenceStore.hpp"
#include "SequencedInboundMessage.hpp"

// Core is generic over the command and asks a few things of it, checked by building Core's own templates
// with ours. Nothing runs here: the gateway needs sockets.
template class NetworkGateway<BatchProtocolHandler, BatchCommand>;
template class SequenceStore<BatchCommand>;

static_assert(std::is_same_v<decltype(BatchCommand::trace_id), TraceId>, "the gateway writes trace_id");
static_assert(std::is_same_v<decltype(BatchCommand::sessionId), SessionId>, "the gateway writes sessionId");
static_assert(std::is_same_v<decltype(BatchEvent::session_id), SessionId>, "egress routes by session_id");
static_assert(std::is_trivially_copyable_v<SequencedInboundMessage<BatchCommand>>);
static_assert(std::is_trivially_copyable_v<BatchEvent>, "events cross the egress as raw bytes");
static_assert(sizeof(SequencedInboundMessage<BatchCommand>) <= 1400, "one datagram, no IP fragmentation");
static_assert(sizeof(EgressBatchHeader) + sizeof(BatchEvent) <= 1400, "an event and its header fit one datagram");
static_assert(EventsPerEgressDatagram<BatchEvent>(1400) > 1, "an egress datagram carries a batch of events");
static_assert(std::is_same_v<decltype(std::declval<const BatchProtocolHandler&>().validateAndParse(
                                 static_cast<const char*>(nullptr), std::size_t{}, std::declval<BatchCommand&>())),
                             std::size_t>,
              "the contract of NetworkGateway");

namespace {

constexpr std::size_t FATAL = BatchProtocolHandler::PARSE_FATAL_ERROR;
const BatchProtocolHandler handler;

std::size_t parse(const std::vector<char>& frame, BatchCommand& out) {
    return handler.validateAndParse(atEdgeOfPage(frame.data(), frame.size()), frame.size(), out);
}

std::size_t parsePrefix(const std::vector<char>& frame, std::size_t length, BatchCommand& out) {
    return handler.validateAndParse(atEdgeOfPage(frame.data(), length), length, out);
}

std::vector<char> bytes(std::initializer_list<int> values) {
    std::vector<char> out;
    for (const int v : values) out.push_back(static_cast<char>(v));
    return out;
}

std::vector<char> rawFrame(std::uint16_t declaredLength, char type) {
    std::vector<char> out;
    wire::putU16(out, declaredLength);
    out.push_back(static_cast<char>(wire::PROTOCOL_VERSION));
    out.push_back(type);
    out.insert(out.end(), declaredLength > 2 ? declaredLength - 2u : 0u, 0);
    return out;
}

void expectSameOrder(const BatchCommand& got, const BatchCommand& want) {
    EXPECT_EQ(got.participantId, want.participantId);
    EXPECT_EQ(got.orderToken, want.orderToken);
    EXPECT_EQ(got.side, want.side);
    EXPECT_EQ(got.acceleratorMask, want.acceleratorMask);
    EXPECT_EQ(got.interconnectMask, want.interconnectMask);
    EXPECT_EQ(got.regionMask, want.regionMask);
    EXPECT_EQ(got.baseHour, want.baseHour);
    EXPECT_EQ(got.slotMask, want.slotMask);
    EXPECT_EQ(got.perSlotCap, want.perSlotCap);
    EXPECT_EQ(got.stepCount, want.stepCount);
    for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) {
        EXPECT_EQ(got.steps[k].quantity, want.steps[k].quantity) << "step " << k;
        EXPECT_EQ(got.steps[k].price, want.steps[k].price) << "step " << k;
    }
}

BatchCommand randomOrder(std::mt19937_64& rng) {
    auto below = [&](std::uint64_t n) { return rng() % n; };
    BatchCommand c = below(2) ? validBid() : validAsk();
    c.participantId = static_cast<ParticipantId>(rng());
    c.orderToken = rng();
    c.baseHour = static_cast<HourIndex>(rng());
    const std::size_t spoils = below(4);
    for (std::size_t s = 0; s < spoils; ++s) {
        switch (below(9)) {
        case 0: c.side = static_cast<Side>(below(256)); break;
        case 1: c.acceleratorMask = static_cast<std::uint32_t>(rng() & (below(2) ? 0x1F : 0xFFFFFFFF)); break;
        case 2: c.interconnectMask = static_cast<std::uint8_t>(rng() & (below(2) ? 0x3 : 0xFF)); break;
        case 3: c.regionMask = static_cast<std::uint32_t>(rng() & (below(2) ? 0x7 : 0xFFFFFFFF)); break;
        case 4: c.slotMask = rng() & (below(2) ? ((std::uint64_t{ 1 } << 47) - 1) : ~std::uint64_t{ 0 }); break;
        case 5: c.perSlotCap = static_cast<NodeUnits>(below(2) ? below(1100) : rng()); break;
        case 6: c.stepCount = static_cast<std::uint8_t>(below(7)); break;
        case 7: c.steps[below(4)].quantity = static_cast<NodeUnits>(below(2) ? below(50'000) : rng()); break;
        default: c.steps[below(4)].price = static_cast<PriceTicks>(below(2) ? below(2'000'000) : rng()); break;
        }
    }
    return c;
}

} // namespace

TEST(ProtocolHandler, CutsTheStreamIntoFramesAndRefusesAFrameThatCanNeverBeValid) {
    {
        const std::vector<char> frame = wire::encodeOrder(validBid());
        ASSERT_EQ(frame.size(), wire::LENGTH_FIELD_SIZE + wire::ORDER_PAYLOAD_SIZE);
        for (std::size_t have = 0; have < frame.size(); ++have) {
            BatchCommand cmd{};
            EXPECT_EQ(parsePrefix(frame, have, cmd), 0u) << "with " << have << " of " << frame.size() << " bytes";
        }
        BatchCommand cmd{};
        EXPECT_EQ(parse(frame, cmd), frame.size());
        EXPECT_EQ(cmd.type, CommandType::ORDER);
    }

    {
        std::vector<char> stream = wire::encodeCancel(3, 77);
        const std::size_t frameLen = stream.size();
        stream.insert(stream.end(), { 'x', 'y', 'z' });
        BatchCommand cmd{};
        EXPECT_EQ(parse(stream, cmd), frameLen);
        EXPECT_EQ(cmd.type, CommandType::CANCEL);
    }

    // The loop of NetworkGateway: parse, and consume what was parsed.
    {
        std::vector<char> stream;
        for (const auto& frame : { wire::encodeOrder(validBid()), wire::encodeCancel(7, 1001), wire::encodeBatchClear(5),
                                   wire::encodeOrder(validAsk()) }) {
            stream.insert(stream.end(), frame.begin(), frame.end());
        }
        std::vector<CommandType> types;
        std::size_t at = 0;
        while (at < stream.size()) {
            BatchCommand cmd{};
            const std::size_t used = handler.validateAndParse(stream.data() + at, stream.size() - at, cmd);
            ASSERT_NE(used, FATAL);
            ASSERT_GT(used, 0u);
            types.push_back(cmd.type);
            at += used;
        }
        EXPECT_EQ(at, stream.size());
        ASSERT_EQ(types.size(), 4u);
        EXPECT_EQ(types[0], CommandType::ORDER);
        EXPECT_EQ(types[1], CommandType::CANCEL);
        EXPECT_EQ(types[2], CommandType::BATCH_CLEAR);
        EXPECT_EQ(types[3], CommandType::ORDER);
    }

    // The gateway appends what it reads to a buffer and parses until nothing more comes out.
    {
        std::vector<char> stream;
        for (const auto& frame : { wire::encodeOrder(validAsk()), wire::encodeBatchClear(9), wire::encodeCancel(2, 3) }) {
            stream.insert(stream.end(), frame.begin(), frame.end());
        }
        std::vector<char> buffer;
        std::vector<BatchCommand> commands;
        for (const char byte : stream) {
            buffer.push_back(byte);
            while (true) {
                BatchCommand cmd{};
                const std::size_t used = handler.validateAndParse(buffer.data(), buffer.size(), cmd);
                ASSERT_NE(used, FATAL);
                if (used == 0) break;
                commands.push_back(cmd);
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(used));
            }
        }
        EXPECT_TRUE(buffer.empty());
        ASSERT_EQ(commands.size(), 3u);
        EXPECT_EQ(commands[0].type, CommandType::ORDER);
        EXPECT_EQ(commands[0].orderToken, validAsk().orderToken);
        EXPECT_EQ(commands[1].type, CommandType::BATCH_CLEAR);
        EXPECT_EQ(commands[1].batchId, 9u);
        EXPECT_EQ(commands[2].type, CommandType::CANCEL);
        EXPECT_EQ(commands[2].orderToken, 3u);
    }

    BatchCommand cmd{};
    EXPECT_EQ(parse(bytes({ 0, 0 }), cmd), FATAL);
    EXPECT_EQ(parse(bytes({ 0, 0, 'O', 'O' }), cmd), FATAL);

    for (const char type : { 'Z', 'o', 'x', 'c', '\0', '\x7F', static_cast<char>(0xFF), 'U', 'B', 'A' }) {
        EXPECT_EQ(parse(rawFrame(5, type), cmd), FATAL) << "type byte " << int(type);
    }

    // The version comes first and says how to read the rest: any other is refused, and at once.
    for (const std::vector<char>& original :
         { wire::encodeOrder(validBid()), wire::encodeCancel(1, 2), wire::encodeBatchClear(3) }) {
        ASSERT_EQ(static_cast<std::uint8_t>(original[wire::LENGTH_FIELD_SIZE]), wire::PROTOCOL_VERSION);
        EXPECT_EQ(parsePrefix(original, wire::LENGTH_FIELD_SIZE + 1, cmd), 0u) << "the right version waits for the rest";
        for (int version = 0; version < 256; ++version) {
            if (version == wire::PROTOCOL_VERSION) continue;
            std::vector<char> frame = original;
            frame[wire::LENGTH_FIELD_SIZE] = static_cast<char>(version);
            EXPECT_EQ(parse(frame, cmd), FATAL) << "version " << version;
            EXPECT_EQ(parsePrefix(frame, wire::LENGTH_FIELD_SIZE + 1, cmd), FATAL)
                << "version " << version << " with nothing after it";
        }
    }

    struct Shape {
        char type;
        std::size_t right;
    };
    for (const Shape s : { Shape{ wire::ORDER, wire::ORDER_PAYLOAD_SIZE }, Shape{ wire::CANCEL, wire::CANCEL_PAYLOAD_SIZE },
                           Shape{ wire::BATCH_CLEAR, wire::BATCH_CLEAR_PAYLOAD_SIZE } }) {
        for (const std::size_t wrong : { s.right - 1, s.right + 1, std::size_t{ 1 } }) {
            EXPECT_EQ(parse(rawFrame(static_cast<std::uint16_t>(wrong), s.type), cmd), FATAL)
                << s.type << " with a payload of " << wrong;
        }
    }

    // Fatal at once, without waiting for the rest: the gateway keeps an incomplete frame in a buffer with
    // no limit of its own.
    for (const std::size_t declared : { wire::MAX_PAYLOAD_SIZE + 1, std::size_t{ 200 }, std::size_t{ 4096 },
                                        std::size_t{ 65535 } }) {
        EXPECT_EQ(parse(bytes({ static_cast<int>(declared >> 8), static_cast<int>(declared & 0xFF) }), cmd), FATAL)
            << "declared " << declared << ", and only the length field has arrived";
    }
    EXPECT_EQ(parse(bytes({ 0, static_cast<int>(wire::MAX_PAYLOAD_SIZE) }), cmd), 0u);
}

TEST(ProtocolHandler, DecodesEveryMessageFieldByFieldFromTheRightBytes) {
    // Distinctive values in every byte of every field. The order is not valid (the masks are far beyond the
    // catalog), but an invalid order is still decoded in full, so a byte read from the wrong place or in
    // the wrong order shows up.
    {
        BatchCommand sent{};
        sent.participantId = 0x01020304;
        sent.orderToken = 0x1112131415161718ull;
        sent.side = Side::ASK;
        sent.acceleratorMask = 0xA1A2A3A4;
        sent.interconnectMask = 0xB1;
        sent.regionMask = 0xC1C2C3C4;
        sent.baseHour = 0x0A0B0C0D;
        sent.slotMask = 0x2122232425262728ull;
        sent.perSlotCap = 0x31323334;
        sent.stepCount = 0x41;
        for (std::size_t k = 0; k < limits::MAX_STEPS; ++k) {
            sent.steps[k].quantity = 0x50515253u + static_cast<std::uint32_t>(k) * 0x01010101u;
            sent.steps[k].price = 0x60616263u + static_cast<std::uint32_t>(k) * 0x01010101u;
        }
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeOrder(sent);
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.type, CommandType::INVALID);
        expectSameOrder(got, sent);
    }

    // A field the type does not use stays zero.
    for (const BatchCommand& sent : { validBid(), validAsk() }) {
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeOrder(sent);
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.type, CommandType::ORDER);
        EXPECT_EQ(got.invalidReason, RejectReason::NONE);
        EXPECT_EQ(got.invalidDetail, 0u);
        expectSameOrder(got, sent);
        EXPECT_EQ(got.batchId, 0u);
    }

    // The gateway writes the trace id and the session after the handler returns, so the handler neither
    // mints nor keeps either.
    {
        BatchCommand got{};
        got.trace_id = 123;
        got.sessionId = 456;
        const std::vector<char> frame = wire::encodeOrder(validBid());
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.trace_id, 0u);
        EXPECT_EQ(got.sessionId, 0u);
    }

    {
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeCancel(0xDEADBEEF, 0x0102030405060708ull);
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.type, CommandType::CANCEL);
        EXPECT_EQ(got.participantId, 0xDEADBEEFu);
        EXPECT_EQ(got.orderToken, 0x0102030405060708ull);
        EXPECT_EQ(got.batchId, 0u);
        EXPECT_EQ(got.acceleratorMask, 0u);
        EXPECT_EQ(got.stepCount, 0u);
    }

    // Any batch clear is accepted, whatever its id: zero, a repeat, one that goes backwards, the largest.
    for (const BatchId id : { BatchId{ 0 }, BatchId{ 1 }, BatchId{ 1 }, BatchId{ 0 }, BatchId{ 0x0102030405060708ull },
                              ~BatchId{ 0 } }) {
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeBatchClear(id);
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.type, CommandType::BATCH_CLEAR);
        EXPECT_EQ(got.batchId, id);
    }

    // The reserved workload class byte is read and ignored: whatever a client puts there, the order is
    // judged by the same checks and the command carries zero.
    for (int value = 0; value < 256; ++value) {
        for (const BatchCommand& sent : { validBid(), validAsk() }) {
            BatchCommand got{};
            const std::vector<char> frame = wire::encodeOrder(sent, static_cast<std::uint8_t>(value));
            ASSERT_EQ(parse(frame, got), frame.size());
            EXPECT_EQ(got.type, CommandType::ORDER) << "workload class " << value;
            EXPECT_EQ(got.workloadClass, 0u);
            expectSameOrder(got, sent);
        }
    }
}

TEST(ProtocolHandler, AnOrderThatFailsACheckBecomesAnInvalidCommandThatKeepsItsOwnerAndToken) {
    // Every kind of problem, one at a time: the command says which, and whose order it was, because the
    // engine refuses it with an event addressed to its owner.
    struct Case {
        const char* what;
        void (*spoil)(BatchCommand&);
        RejectReason reason;
        std::uint16_t detail;
    };
    const Case cases[] = {
        { "side", [](BatchCommand& c) { c.side = static_cast<Side>('Q'); }, RejectReason::BAD_SIDE, 'Q' },
        { "empty mask", [](BatchCommand& c) { c.regionMask = 0; }, RejectReason::EMPTY_MASK,
          maskDetail(catalog::Attribute::REGION, 0) },
        { "mask bit beyond the catalog", [](BatchCommand& c) { c.slotMask |= std::uint64_t{ 1 } << 50; },
          RejectReason::MASK_BIT_OUT_OF_RANGE, maskDetail(catalog::Attribute::SLOTS, 50) },
        { "per-slot cap", [](BatchCommand& c) { c.perSlotCap = 0; }, RejectReason::PER_SLOT_CAP_OUT_OF_RANGE, 0 },
        { "step count", [](BatchCommand& c) { c.stepCount = 9; }, RejectReason::STEP_COUNT_OUT_OF_RANGE, 9 },
        { "step quantity", [](BatchCommand& c) { c.steps[1].quantity = 0; }, RejectReason::STEP_QUANTITY_ZERO, 1 },
        { "step price", [](BatchCommand& c) { c.steps[2].price = 0xFFFFFFFFu; }, RejectReason::STEP_PRICE_OVER_MAX, 2 },
        { "price order", [](BatchCommand& c) { c.steps[1].price = 9999; }, RejectReason::STEP_PRICE_ORDER, 1 },
        { "total", [](BatchCommand& c) { c.steps[2].quantity = 47'000; }, RejectReason::TOTAL_QUANTITY_OVER_MAX, 2 },
    };
    for (const Case& c : cases) {
        BatchCommand sent = validBid();
        c.spoil(sent);
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeOrder(sent);
        ASSERT_EQ(parse(frame, got), frame.size()) << c.what << ": a bad order still consumes its frame";
        EXPECT_EQ(got.type, CommandType::INVALID) << c.what;
        EXPECT_EQ(got.invalidReason, c.reason) << c.what;
        EXPECT_EQ(got.invalidDetail, c.detail) << c.what;
        EXPECT_EQ(got.participantId, sent.participantId) << c.what;
        EXPECT_EQ(got.orderToken, sent.orderToken) << c.what;
    }
    {
        BatchCommand ask = validAsk();
        ask.acceleratorMask = 0b11;
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeOrder(ask);
        ASSERT_EQ(parse(frame, got), frame.size());
        EXPECT_EQ(got.type, CommandType::INVALID);
        EXPECT_EQ(got.invalidReason, RejectReason::ASK_MASK_NOT_SINGLE);
        EXPECT_EQ(got.invalidDetail, maskDetail(catalog::Attribute::ACCELERATOR, 0));
    }

    // On random orders: invalid exactly when the checks refuse it and for the same reason, and either way
    // its fields arrive as they were sent.
    std::mt19937_64 rng(31337);
    std::size_t valid = 0;
    std::size_t invalid = 0;
    for (std::size_t i = 0; i < 50'000; ++i) {
        const BatchCommand sent = randomOrder(rng);
        BatchCommand got{};
        const std::vector<char> frame = wire::encodeOrder(sent);
        ASSERT_EQ(parse(frame, got), frame.size());
        const Reject r = checkOrder(sent);
        if (r) {
            ASSERT_EQ(got.type, CommandType::INVALID) << "order " << i;
            ASSERT_EQ(got.invalidReason, r.reason) << "order " << i;
            ASSERT_EQ(got.invalidDetail, r.detail) << "order " << i;
            ++invalid;
        } else {
            ASSERT_EQ(got.type, CommandType::ORDER) << "order " << i;
            ASSERT_EQ(got.invalidReason, RejectReason::NONE) << "order " << i;
            ++valid;
        }
        expectSameOrder(got, sent);
        ASSERT_EQ(got.participantId, sent.participantId);
        ASSERT_EQ(got.orderToken, sent.orderToken);
    }
    EXPECT_GT(valid, 2000u);
    EXPECT_GT(invalid, 2000u);
}

TEST(ProtocolHandler, GarbageNeverMakesItReadPastWhatItWasGivenOrClaimMoreThanItHas) {
    std::mt19937_64 rng(424242);
    std::size_t framesParsed = 0;
    for (std::size_t i = 0; i < 60'000; ++i) {
        const char type = "OXC"[rng() % 3];
        const std::size_t shape = rng() % 4;
        std::size_t length = rng() % 90;
        if (shape == 0) {
            // A whole frame of the right size for its type, perhaps with a few bytes after it.
            const std::size_t payload = type == wire::ORDER   ? wire::ORDER_PAYLOAD_SIZE
                                        : type == wire::CANCEL ? wire::CANCEL_PAYLOAD_SIZE
                                                               : wire::BATCH_CLEAR_PAYLOAD_SIZE;
            length = wire::LENGTH_FIELD_SIZE + payload + rng() % 4;
        }
        std::vector<char> data(length);
        for (char& b : data) b = static_cast<char>(rng());
        // Often start from something that looks like a frame, so that the body gets read.
        if (length >= 4 && shape <= 1) {
            const std::size_t payload = shape == 0
                                            ? (type == wire::ORDER   ? wire::ORDER_PAYLOAD_SIZE
                                               : type == wire::CANCEL ? wire::CANCEL_PAYLOAD_SIZE
                                                                      : wire::BATCH_CLEAR_PAYLOAD_SIZE)
                                            : length - wire::LENGTH_FIELD_SIZE;
            data[0] = static_cast<char>(payload >> 8);
            data[1] = static_cast<char>(payload);
            data[2] = static_cast<char>(wire::PROTOCOL_VERSION);
            data[3] = type;
        }
        BatchCommand cmd{};
        const std::size_t used = parse(data, cmd);
        ASSERT_TRUE(used == FATAL || used <= length) << "claimed " << used << " of " << length << " bytes";
        if (used != FATAL && used > 0) {
            ++framesParsed;
            const std::size_t declared = (static_cast<unsigned char>(data[0]) << 8) | static_cast<unsigned char>(data[1]);
            ASSERT_EQ(used, wire::LENGTH_FIELD_SIZE + declared) << "what it consumes is the frame it was told about";
        }
    }
    EXPECT_GT(framesParsed, 1000u) << "the garbage must also include frames that parse";

    for (const std::vector<char>& original : { wire::encodeOrder(validBid()), wire::encodeOrder(validAsk()),
                                               wire::encodeCancel(5, 6), wire::encodeBatchClear(7) }) {
        for (std::size_t at = 0; at < original.size(); ++at) {
            for (const int flip : { 0x01, 0x80, 0xFF }) {
                std::vector<char> mutated = original;
                mutated[at] = static_cast<char>(mutated[at] ^ flip);
                BatchCommand cmd{};
                const std::size_t used = parse(mutated, cmd);
                EXPECT_TRUE(used == FATAL || used <= mutated.size()) << "byte " << at;
            }
        }
    }
}

TEST(EventFrame, PutsEveryFieldOfEveryEventWhereTheLayoutSaysAndRefusesWhatIsNotOneFrame) {
    std::mt19937_64 rng(20261009);
    // A different value in every field, so that two fields that swapped places would be noticed.
    const auto sample = [&](EventType type) {
        BatchEvent e{};
        e.type = type;
        e.sequence_number = rng();
        e.batchId = rng();
        e.participantId = static_cast<ParticipantId>(rng());
        e.orderToken = rng();
        e.side = Side::ASK;
        e.rank = static_cast<std::uint32_t>(rng());
        e.rejectReason = RejectReason::BATCH_FULL;
        e.rejectDetail = static_cast<std::uint16_t>(rng());
        e.accelerator = static_cast<std::uint8_t>(rng());
        e.interconnect = static_cast<std::uint8_t>(rng());
        e.region = static_cast<std::uint8_t>(rng());
        e.lifted = static_cast<std::uint8_t>(rng());
        e.hour = static_cast<HourIndex>(rng());
        e.price = static_cast<PriceTicks>(rng());
        e.units = static_cast<NodeUnits>(rng());
        e.unfilledUnits = static_cast<NodeUnits>(rng());
        e.liftedUnits = static_cast<NodeUnits>(rng());
        e.amount = -static_cast<std::int64_t>(rng() >> 2);
        e.rankSum = -static_cast<std::int64_t>(rng() >> 2);
        e.fills = static_cast<std::uint32_t>(rng());
        e.bids = static_cast<std::uint32_t>(rng());
        e.asks = static_cast<std::uint32_t>(rng());
        e.arcs = static_cast<std::uint32_t>(rng());
        e.pivots = static_cast<std::uint32_t>(rng());
        e.certified = static_cast<std::uint8_t>(rng());
        e.voidReason = VoidReason::SOLVER_ERROR;
        return e;
    };
    const auto decode = [](const std::vector<char>& frame, std::size_t length) {
        return wire::decodeEvent(atEdgeOfPage(frame.data(), length), length);
    };

    const char letters[] = { 'A', 'J', 'C', 'F', 'D', 'B', 'V' };
    std::size_t k = 0;
    for (const EventType type : wire::EVENT_TYPES) {
        SCOPED_TRACE(toString(type));
        const BatchEvent e = sample(type);
        const std::vector<char> frame = wire::encodeEvent(e);
        ASSERT_EQ(frame.size(), wire::EVENT_FRAME_SIZE);
        EXPECT_EQ(frame[3], letters[k++]);

        const std::optional<BatchEvent> back = decode(frame, frame.size());
        ASSERT_TRUE(back);
        EXPECT_TRUE(enginefix::sameEvent(*back, e)) << enginefix::describe(*back) << "\n" << enginefix::describe(e);

        BatchEvent elsewhere = e;
        elsewhere.session_id = 12345;
        EXPECT_EQ(wire::encodeEvent(elsewhere), frame) << "the session routes the frame and is not in it";
    }

    {
        SCOPED_TRACE("the places of the fields, as the layout in BatchEventFrame.hpp gives them");
        const BatchEvent e = sample(EventType::FILL);
        const std::vector<char> frame = wire::encodeEvent(e);
        const auto field = [&](std::size_t offset, std::size_t size) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < size; ++i) value = (value << 8) | static_cast<std::uint8_t>(frame[2 + offset + i]);
            return value;
        };
        EXPECT_EQ(static_cast<std::uint8_t>(frame[0]) * 256u + static_cast<std::uint8_t>(frame[1]), 100u)
            << "the length of what follows the length";
        EXPECT_EQ(field(0, 1), wire::PROTOCOL_VERSION);
        EXPECT_EQ(field(1, 1), 'F');
        EXPECT_EQ(field(2, 8), e.sequence_number);
        EXPECT_EQ(field(10, 8), e.batchId);
        EXPECT_EQ(field(18, 4), e.participantId);
        EXPECT_EQ(field(22, 8), e.orderToken);
        EXPECT_EQ(field(30, 1), static_cast<std::uint64_t>('A'));
        EXPECT_EQ(field(31, 4), e.rank);
        EXPECT_EQ(field(35, 1), static_cast<std::uint64_t>(RejectReason::BATCH_FULL));
        EXPECT_EQ(field(36, 2), e.rejectDetail);
        EXPECT_EQ(field(38, 1), e.accelerator);
        EXPECT_EQ(field(39, 1), e.interconnect);
        EXPECT_EQ(field(40, 1), e.region);
        EXPECT_EQ(field(41, 1), e.lifted);
        EXPECT_EQ(field(42, 4), e.hour);
        EXPECT_EQ(field(46, 4), e.price);
        EXPECT_EQ(field(50, 4), e.units);
        EXPECT_EQ(field(54, 4), e.unfilledUnits);
        EXPECT_EQ(field(58, 4), e.liftedUnits);
        EXPECT_EQ(field(62, 8), static_cast<std::uint64_t>(e.amount));
        EXPECT_EQ(field(70, 8), static_cast<std::uint64_t>(e.rankSum));
        EXPECT_EQ(field(78, 4), e.fills);
        EXPECT_EQ(field(82, 4), e.bids);
        EXPECT_EQ(field(86, 4), e.asks);
        EXPECT_EQ(field(90, 4), e.arcs);
        EXPECT_EQ(field(94, 4), e.pivots);
        EXPECT_EQ(field(98, 1), e.certified);
        EXPECT_EQ(field(99, 1), static_cast<std::uint64_t>(VoidReason::SOLVER_ERROR));
        EXPECT_EQ(wire::EVENT_FRAME_SIZE, 2u + 100u);
    }

    {
        SCOPED_TRACE("what is not one whole frame of a known type");
        const std::vector<char> frame = wire::encodeEvent(sample(EventType::ORDER_DONE));
        for (std::size_t length = 0; length < frame.size(); ++length) {
            EXPECT_FALSE(decode(frame, length)) << "a frame cut to " << length << " bytes";
        }
        std::vector<char> longer = frame;
        longer.push_back(0);
        EXPECT_FALSE(decode(longer, longer.size()));

        for (const int declared : { 0, 1, 99, 101, 0xFFFF }) {
            std::vector<char> bad = frame;
            bad[0] = static_cast<char>(declared >> 8);
            bad[1] = static_cast<char>(declared);
            EXPECT_FALSE(decode(bad, bad.size())) << "declared length " << declared;
        }
        for (int byte = 0; byte < 256; ++byte) {
            std::vector<char> bad = frame;
            bad[2] = static_cast<char>(byte);
            EXPECT_EQ(decode(bad, bad.size()).has_value(), byte == wire::PROTOCOL_VERSION) << "version byte " << byte;
        }
        for (int byte = 0; byte < 256; ++byte) {
            std::vector<char> bad = frame;
            bad[3] = static_cast<char>(byte);
            const bool known = std::find(std::begin(letters), std::end(letters), static_cast<char>(byte)) != std::end(letters);
            EXPECT_EQ(decode(bad, bad.size()).has_value(), known) << "type byte " << byte;
        }

        // A value from a newer version of the protocol is passed on as it is.
        std::vector<char> newer = frame;
        newer[2 + 30] = 'x';
        newer[2 + 35] = static_cast<char>(200);
        newer[2 + 99] = static_cast<char>(201);
        const std::optional<BatchEvent> got = decode(newer, newer.size());
        ASSERT_TRUE(got);
        EXPECT_EQ(static_cast<char>(got->side), 'x');
        EXPECT_EQ(static_cast<std::uint8_t>(got->rejectReason), 200u);
        EXPECT_EQ(static_cast<std::uint8_t>(got->voidReason), 201u);
    }

    {
        SCOPED_TRACE("the console line");
        for (const EventType type : wire::EVENT_TYPES) {
            BatchEvent e = sample(type);
            e.session_id = 7;
            std::ostringstream line;
            BatchEventEncoder::print(line, e);
            EXPECT_EQ(line.str().find('\n'), std::string::npos);
            EXPECT_EQ(line.str().rfind(toString(type), 0), 0u) << "it starts with the type: " << line.str();
            EXPECT_NE(line.str().find("seq=" + std::to_string(e.sequence_number)), std::string::npos);
            EXPECT_NE(line.str().find("session=7"), std::string::npos);
            if (type == EventType::REJECTED) {
                EXPECT_NE(line.str().find("BATCH_FULL"), std::string::npos) << line.str();
            }
        }
    }
}

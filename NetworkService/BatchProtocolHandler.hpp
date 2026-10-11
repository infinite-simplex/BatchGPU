#pragma once
#include <cstddef>

#include "BatchCommand.hpp"

// Called by Core's NetworkGateway for every chunk of a client's stream. It turns the bytes of a frame
// (BatchWire.hpp) into a BatchCommand and makes the stateless checks on an order. It has no state: the
// checks that need the batch belong to the engine.
//
// What it returns, and what the gateway does with it:
//   - the bytes consumed (more than 0): a whole frame was parsed into `outCmd`. An order that fails a
//     stateless check comes back as an INVALID command carrying the reason and a detail, and flows on
//     through the pipeline so the engine refuses it with an event.
//   - 0: the frame is not all here yet.
//   - PARSE_FATAL_ERROR: the frame cannot be interpreted at all (a length out of range, a protocol version
//     or a type it does not know, a size that does not fit its type), and nothing after it on the stream
//     can be trusted, so the gateway closes the connection.
//
// A BATCH_CLEAR is accepted whatever its id, and the reserved workload-class byte of an order is read
// and ignored.
class BatchProtocolHandler {
public:
    static constexpr std::size_t PARSE_FATAL_ERROR = static_cast<std::size_t>(-1);

    std::size_t validateAndParse(const char* buf, std::size_t availableLen, BatchCommand& outCmd) const;
};

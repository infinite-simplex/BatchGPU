#include "BatchEngine.hpp"
#include "BatchEvent.hpp"
#include "MatchingService.hpp" // Core's replica host
#include "SPSCProducerPolicy.hpp"

// A compile check that Core's MatchingService<TCommand, TEngine> builds against
// BatchEngine::submit_order for this command type, and that the engine builds with the policy a
// replica runs.
template class BatchEngine<SPSCProducerPolicy<BatchEvent>>;
template class MatchingService<BatchCommand, BatchEngine<SPSCProducerPolicy<BatchEvent>>>;

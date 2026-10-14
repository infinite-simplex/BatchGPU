# A command-line order-entry client. It uses the frame encoders and the catalog and schedule headers directly,
# so it cannot drift from what the gateway reads.
add_executable(BatchClient BatchClient.cpp)
target_link_libraries(BatchClient PRIVATE BatchProtocol ExchangeCommon)

add_library(BatchProtocol STATIC
    BatchProtocolHandler.cpp
    BatchTicker.cpp
)
target_include_directories(BatchProtocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BatchProtocol PUBLIC ExchangeCommon)

# The gateway: order entry, the sequenced log and its retransmit server, the replicas' arbiter, the
# results going back to clients, and the ticker. One per exchange.
add_executable(NetworkServiceApp main.cpp)
target_link_libraries(NetworkServiceApp PRIVATE BatchProtocol Core Threads::Threads)

# The engine, with no sockets or threads: the tests drive it in process. MatchingService.cpp is an
# explicit instantiation with Core's replica host, as a compile check.
add_library(BatchMatching STATIC
    BatchBook.cpp
    MatchingService.cpp
)
target_include_directories(BatchMatching PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(BatchMatching PUBLIC Clearing ExchangeCommon Core Threads::Threads)

# A replica: joins the gateway's log, runs the engine, and publishes the results while it is the leader.
add_executable(MatchingServiceApp main.cpp)
target_link_libraries(MatchingServiceApp PRIVATE BatchMatching Core Threads::Threads)

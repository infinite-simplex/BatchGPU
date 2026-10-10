add_library(Recorder STATIC ResultsStore.cpp)
target_include_directories(Recorder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Recorder PUBLIC ExchangeCommon Clearing sqlite3 Core)

# A replica that never leads: it sends no heartbeats, publishes nothing, and writes each batch to SQLite.
add_executable(RecorderApp main.cpp)
target_link_libraries(RecorderApp PRIVATE Recorder BatchMatching Core Threads::Threads)

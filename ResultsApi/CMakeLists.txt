add_library(ResultsApi STATIC
    Http.cpp
    HttpServer.cpp
    ResultsApi.cpp
)
target_include_directories(ResultsApi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ResultsApi PUBLIC Recorder ExchangeCommon Threads::Threads)

# The read-only API: a separate process that opens each recorder's file read-only and serves the results as JSON over HTTP.
add_executable(ResultsApiApp main.cpp)
target_link_libraries(ResultsApiApp PRIVATE ResultsApi Threads::Threads)

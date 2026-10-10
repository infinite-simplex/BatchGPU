# A pure function from orders to fills and prices: no sockets, threads or Core. clear() is network
# simplex; MinCostFlow (successive shortest paths) is the independent reference the tests compare against.
add_library(Clearing STATIC
    BatchClearing.cpp
    ClearingValidation.cpp
    MinCostFlow.cpp
    NetworkSimplex.cpp
    SimplexCore.cpp
)
target_include_directories(Clearing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Clearing PUBLIC ExchangeCommon)

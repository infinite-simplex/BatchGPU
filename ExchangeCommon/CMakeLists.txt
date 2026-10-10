# Header-only. Links Core because the command and event structs include Core's Alias.hpp.
add_library(ExchangeCommon INTERFACE)

target_include_directories(ExchangeCommon INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ExchangeCommon INTERFACE Core)

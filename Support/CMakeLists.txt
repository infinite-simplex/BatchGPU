add_library(Support INTERFACE)
target_include_directories(Support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Support INTERFACE Clearing)

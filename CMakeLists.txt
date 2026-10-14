cmake_minimum_required(VERSION 3.20)
project(BatchGPU C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -Wall -Wextra -pthread")
endif()

find_package(Threads REQUIRED)

include(FetchContent)

# Pinned to a release tag so a Core change can never silently move this repo.
FetchContent_Declare(
  Core
  GIT_REPOSITORY https://github.com/infinite-simplex/ToyExchange.git
  GIT_TAG v0.1.0
  SOURCE_SUBDIR Core
)
FetchContent_MakeAvailable(Core)

# The official amalgamation of SQLite 3.53.4 (public domain), checked against the hash that sqlite.org publishes.
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
)
FetchContent_MakeAvailable(sqlite3)
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0
                                           SQLITE_DEFAULT_MEMSTATUS=0)
target_link_libraries(sqlite3 PUBLIC Threads::Threads)

add_subdirectory(ExchangeCommon)
add_subdirectory(Clearing)
add_subdirectory(Support)
add_subdirectory(NetworkService)
add_subdirectory(MatchingService)
add_subdirectory(Recorder)
add_subdirectory(ResultsApi)
add_subdirectory(Client)

add_executable(ClearingBench Bench/ClearingBench.cpp)
target_link_libraries(ClearingBench PRIVATE Clearing Support Threads::Threads)

enable_testing()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

add_executable(ProductTests
    Tests/ApiTests.cpp
    Tests/BatchBookTests.cpp
    Tests/CatalogTests.cpp
    Tests/ClearingTests.cpp
    Tests/EngineModelTests.cpp
    Tests/EngineTests.cpp
    Tests/ExchangeTests.cpp
    Tests/GeneratorTests.cpp
    Tests/GoldenTests.cpp
    Tests/OrderChecksTests.cpp
    Tests/ProductionSolverTests.cpp
    Tests/ProtocolHandlerTests.cpp
    Tests/RecorderTests.cpp
    Tests/ScheduleTests.cpp
    Tests/SolverAgreementTests.cpp
)
target_link_libraries(ProductTests
    PRIVATE
        gtest_main
        Clearing
        Support
        ExchangeCommon
        BatchProtocol
        BatchMatching
        Recorder
        ResultsApi
        Threads::Threads
)
add_dependencies(ProductTests RecorderApp NetworkServiceApp MatchingServiceApp ResultsApiApp)
target_compile_definitions(ProductTests PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/fixtures"
    RECORDER_APP="$<TARGET_FILE:RecorderApp>"
    GATEWAY_APP="$<TARGET_FILE:NetworkServiceApp>"
    REPLICA_APP="$<TARGET_FILE:MatchingServiceApp>"
    API_APP="$<TARGET_FILE:ResultsApiApp>")

include(GoogleTest)
gtest_discover_tests(ProductTests)

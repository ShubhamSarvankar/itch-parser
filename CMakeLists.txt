cmake_minimum_required(VERSION 3.24)
project(itch_parser VERSION 1.0 LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -Wall -Wextra -Wpedantic -Werror")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native -DNDEBUG")

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.1
)
set(CPPHTTPLIB_OPENSSL_SUPPORT OFF)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
)
set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_MakeAvailable(googlebenchmark)

# HdrHistogram_c for nanosecond latency histograms in bench_replay and
# the realistic-mix benchmark. Optional: build skips it gracefully if
# the FetchContent fetch fails (e.g. offline build).
FetchContent_Declare(hdr_histogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.11.8
)
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hdr_histogram)
if(TARGET hdr_histogram_static)
    set(ITCH_HAVE_HDR ON CACHE INTERNAL "HdrHistogram_c is available")
else()
    set(ITCH_HAVE_HDR OFF CACHE INTERNAL "HdrHistogram_c not available")
endif()

# ankerl::unordered_dense::map - flat hash table, drop-in for std::unordered_map.
# Single header, MIT. 2-5x faster on integer keyed hot paths.
FetchContent_Declare(unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v4.4.0
)
FetchContent_MakeAvailable(unordered_dense)
set(ITCH_HAVE_DENSE_MAP ON CACHE INTERNAL "ankerl::unordered_dense available")

FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG        v1.0.4
)
FetchContent_MakeAvailable(concurrentqueue)

set(CORE_SOURCES
    src/feed_reader.cpp
    src/parser.cpp
    src/book_engine.cpp
    src/snapshot_publisher.cpp
    src/rest_server.cpp
    src/udp_feed_reader.cpp
)

add_library(itch_core STATIC ${CORE_SOURCES})
target_include_directories(itch_core PUBLIC
    include
    ${concurrentqueue_SOURCE_DIR}
)
if(ITCH_HAVE_DENSE_MAP)
    target_link_libraries(itch_core PUBLIC unordered_dense::unordered_dense)
    target_compile_definitions(itch_core PUBLIC ITCH_HAVE_DENSE_MAP=1)
endif()
target_link_libraries(itch_core PUBLIC
    httplib::httplib
    Threads::Threads
    nlohmann_json::nlohmann_json
)

add_executable(itch_parser src/main.cpp)
target_link_libraries(itch_parser PRIVATE itch_core)

add_subdirectory(tests)
add_subdirectory(bench)
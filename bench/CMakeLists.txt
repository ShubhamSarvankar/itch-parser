add_executable(itch_bench
    bench_parser.cpp
    bench_book_engine.cpp
)

target_include_directories(itch_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(itch_bench PRIVATE
    benchmark::benchmark_main
    itch_core
)
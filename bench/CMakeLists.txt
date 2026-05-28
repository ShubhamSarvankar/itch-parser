add_executable(itch_bench
    bench_parser.cpp
    bench_book_engine.cpp
    bench_mixed_workload.cpp
    bench_snapshot.cpp
    bench_ladder_shootout.cpp
)

target_include_directories(itch_bench PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
)

target_link_libraries(itch_bench PRIVATE
    benchmark::benchmark_main
    itch_core
)

# End to end replay harness. Separate binary so perf counters are not
# polluted by the REST server thread or gbench infrastructure.
add_executable(bench_replay bench_replay.cpp)
target_include_directories(bench_replay PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}
)
target_link_libraries(bench_replay PRIVATE itch_core)
if(ITCH_HAVE_HDR)
    target_compile_definitions(bench_replay PRIVATE ITCH_HAVE_HDR=1)
    target_link_libraries(bench_replay PRIVATE hdr_histogram_static)
endif()

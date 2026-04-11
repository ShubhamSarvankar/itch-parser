add_executable(itch_tests
    test_price.cpp
    test_parser.cpp
    test_book_engine.cpp
    test_rest.cpp
    test_udp_feed_reader.cpp
)

target_include_directories(itch_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(itch_tests PRIVATE
    gtest_main
    itch_core
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample.bin
    ${CMAKE_BINARY_DIR}/tests/fixtures/sample.bin
    COPYONLY
)

include(GoogleTest)
gtest_discover_tests(itch_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
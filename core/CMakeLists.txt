# Core library sources (exclude the CLI entry point, which is only linked into
# the executable).
file(GLOB_RECURSE CORE_SOURCES src/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")
file(GLOB_RECURSE CORE_HEADERS include/marketstate/*.hpp)

add_library(marketstate_lib ${CORE_SOURCES} ${CORE_HEADERS})
target_include_directories(marketstate_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(marketstate_lib PRIVATE -Wall -Wextra)
target_link_libraries(marketstate_lib PUBLIC
  SQLite3::SQLite3
  Threads::Threads
)

# Main executable
add_executable(marketstate
  src/main.cpp
)
target_link_libraries(marketstate PRIVATE marketstate_lib)

# Tests
if(BUILD_TESTS)
  file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
  add_executable(marketstate_tests ${TEST_SOURCES})
  target_include_directories(marketstate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(marketstate_tests PRIVATE
    marketstate_lib
    GTest::GTest
    GTest::Main
  )
  enable_testing()
  add_test(NAME marketstate_tests COMMAND marketstate_tests)
endif()

# Benchmarks
if(BUILD_BENCHMARKS)
  file(GLOB_RECURSE BENCH_SOURCES benchmarks/*.cpp)
  if(BENCH_SOURCES)
    add_executable(marketstate_bench ${BENCH_SOURCES})
    target_link_libraries(marketstate_bench PRIVATE
      marketstate_lib
      benchmark::benchmark
      benchmark::benchmark_main
    )
  endif()
endif()

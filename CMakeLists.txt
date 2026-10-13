cmake_minimum_required(VERSION 3.20)
project(MarketState VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configuration
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)
option(ENABLE_SANITIZERS "Enable sanitizers for debug" OFF)

# Required at the top level so ctest discovers tests registered in subdirectories.
enable_testing()

if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# Find dependencies
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

if(BUILD_TESTS)
  find_package(GTest REQUIRED)
endif()

if(BUILD_BENCHMARKS)
  find_package(benchmark REQUIRED)
endif()

# Add subdirectories
add_subdirectory(core)

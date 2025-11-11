cmake_minimum_required(VERSION 3.20)
project(Artemis VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /W3")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
endif()

# Platform-specific settings
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Fetch spdlog
include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
)
FetchContent_MakeAvailable(spdlog)

# Fetch GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
FetchContent_MakeAvailable(googletest)

# Source files
set(SOURCES
    src/MarketDataReader.cpp
    src/RollingStatistics.cpp
    src/SignalGenerator.cpp
    src/Backtester.cpp
    src/Performance.cpp
    src/main.cpp
)

set(HEADERS
    src/MarketDataReader.hpp
    src/RollingStatistics.hpp
    src/SignalGenerator.hpp
    src/Backtester.hpp
    src/Performance.hpp
    src/LockFreeQueue.hpp
)

# Main executable
add_executable(artemis ${SOURCES} ${HEADERS})
target_link_libraries(artemis PRIVATE spdlog::spdlog)
target_include_directories(artemis PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Platform-specific linking
if(UNIX AND NOT APPLE)
    target_link_libraries(artemis PRIVATE pthread)
endif()

# Test executable
enable_testing()

set(TEST_SOURCES
    tests/test_ewma.cpp
    tests/test_lockfree_queue.cpp
    tests/test_signal_generator.cpp
    tests/test_market_data_reader.cpp
)

add_executable(artemis_tests ${TEST_SOURCES} ${SOURCES})
target_link_libraries(artemis_tests PRIVATE 
    GTest::gtest 
    GTest::gtest_main 
    GTest::gmock 
    GTest::gmock_main
    spdlog::spdlog
)
target_include_directories(artemis_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

if(UNIX AND NOT APPLE)
    target_link_libraries(artemis_tests PRIVATE pthread)
endif()

# Add tests
add_test(NAME ArtemisTests COMMAND artemis_tests)

# Coverage (optional)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
    if(ENABLE_COVERAGE)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    endif()
endif()

# Install rules
install(TARGETS artemis DESTINATION bin)


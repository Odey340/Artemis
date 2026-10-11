cmake_minimum_required(VERSION 3.20)
project(Artemis VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ARTEMIS_NATIVE   "Optimise for the build machine's CPU (-march=native)" ON)
option(ARTEMIS_LTO      "Enable link-time optimisation in Release builds" ON)
option(ARTEMIS_TESTS    "Build the GoogleTest suite" ON)
option(ARTEMIS_SANITIZE "Build with address + undefined-behaviour sanitizers" OFF)

# ---------------------------------------------------------------- dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(json spdlog)

# ------------------------------------------------------------------- warnings
if(MSVC)
    set(ARTEMIS_WARNINGS /W4 /permissive- /EHsc /utf-8 /D_WIN32_WINNT=0x0601 /DNOMINMAX
        /D_CRT_SECURE_NO_WARNINGS)
else()
    set(ARTEMIS_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(ARTEMIS_NATIVE AND NOT CMAKE_CROSSCOMPILING)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
        if(HAS_MARCH_NATIVE)
            add_compile_options($<$<CONFIG:Release>:-march=native>)
        endif()
    endif()
endif()

if(ARTEMIS_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------ core library
set(ARTEMIS_SOURCES
    src/core/FixedPoint.cpp
    src/core/OrderBook.cpp
    src/core/RollingStatistics.cpp
    src/core/Clock.cpp
    src/core/Config.cpp
    src/net/HttpClient.cpp
    src/venues/Fees.cpp
    src/venues/kalshi/KalshiSigner.cpp
    src/venues/kalshi/KalshiClient.cpp
    src/venues/polymarket/PolymarketClient.cpp
    src/strategy/ArbDetector.cpp
    src/risk/RiskManager.cpp
    src/exec/OrderGateway.cpp
    src/exec/BasketExecutor.cpp
    src/engine/Universe.cpp
    src/engine/Journal.cpp
    src/engine/Engine.cpp
    src/backtest/Performance.cpp
    src/backtest/Backtester.cpp
)

add_library(artemis_core STATIC ${ARTEMIS_SOURCES})
target_include_directories(artemis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(artemis_core PRIVATE ${ARTEMIS_WARNINGS})
target_link_libraries(artemis_core PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    Threads::Threads
)

add_executable(artemis src/main.cpp)
target_compile_options(artemis PRIVATE ${ARTEMIS_WARNINGS})
target_link_libraries(artemis PRIVATE artemis_core)

if(ARTEMIS_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ARTEMIS_IPO_OK OUTPUT ARTEMIS_IPO_MSG LANGUAGES CXX)
    if(ARTEMIS_IPO_OK)
        set_property(TARGET artemis_core artemis PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endif()

install(TARGETS artemis DESTINATION bin)

# ------------------------------------------------------------------------- tests
if(ARTEMIS_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(artemis_tests
        tests/test_fixed_point.cpp
        tests/test_orderbook.cpp
        tests/test_fees.cpp
        tests/test_ewma.cpp
        tests/test_lockfree_queue.cpp
        tests/test_signal_generator.cpp
        tests/test_arb_detector.cpp
        tests/test_executor.cpp
        tests/test_risk.cpp
        tests/test_kalshi_client.cpp
        tests/test_signer.cpp
        tests/test_universe.cpp
        tests/test_config.cpp
        tests/test_backtest.cpp
    )
    target_compile_options(artemis_tests PRIVATE ${ARTEMIS_WARNINGS})
    target_link_libraries(artemis_tests PRIVATE artemis_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(artemis_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

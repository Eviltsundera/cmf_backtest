cmake_minimum_required(VERSION 3.20)

project(
  lob_backtester
  VERSION 0.1.0
  DESCRIPTION "Event-driven LOB backtest engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "" FORCE)
endif()

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  yaml-cpp
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.zip
  URL_HASH SHA256=334e80ab7b52e14c23f94e041c74bab0742f2281aad55f66be2f19f4b7747071
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(
  spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.zip
  URL_HASH SHA256=b74274c32c8be5dba70b7006c1d41b7d3e5ff0dff8390c8b6390c1189424e094
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(yaml-cpp spdlog googletest)

if(TARGET yaml-cpp::yaml-cpp)
  set(LOB_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(LOB_YAML_TARGET yaml-cpp)
endif()

add_library(
  lob_core STATIC
  src/lob/book/OrderBook.cpp
  src/lob/data/CsvDataSource.cpp
  src/lob/engine/BacktestEngine.cpp
  src/lob/execution/FillModel.cpp
  src/lob/execution/OrderManager.cpp
  src/lob/features/FeatureEngine.cpp
  src/lob/metrics/MetricsEngine.cpp
  src/lob/portfolio/Portfolio.cpp
  src/lob/strategies/Strategy.cpp
  src/lob/utils/Config.cpp
  src/lob/utils/GitMetadata.cpp)
add_library(lob::core ALIAS lob_core)

target_include_directories(lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lob_core PUBLIC ${LOB_YAML_TARGET} spdlog::spdlog)

target_compile_options(
  lob_core
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

add_executable(lob_backtest apps/lob_backtest.cpp)
target_link_libraries(lob_backtest PRIVATE lob::core)
target_compile_options(
  lob_backtest
  PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>)

include(CTest)
if(BUILD_TESTING)
  add_executable(
    lob_tests
    tests/avellaneda_stoikov_strategy_test.cpp
    tests/backtest_engine_test.cpp
    tests/config_smoke_test.cpp
    tests/data_source_test.cpp
    tests/fill_model_test.cpp
    tests/fixed_spread_strategy_test.cpp
    tests/feature_engine_test.cpp
    tests/git_metadata_test.cpp
    tests/metrics_engine_test.cpp
    tests/order_manager_test.cpp
    tests/order_book_test.cpp
    tests/portfolio_test.cpp)
  target_link_libraries(lob_tests PRIVATE lob::core GTest::gtest_main)
  set(LOB_TEST_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/configs")
  set(LOB_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../data/sample")
  target_compile_definitions(
    lob_tests
    PRIVATE
      LOB_TEST_CONFIG_DIR="${LOB_TEST_CONFIG_DIR}"
      LOB_TEST_DATA_DIR="${LOB_TEST_DATA_DIR}"
  )
  target_compile_options(
    lob_tests
    PRIVATE
      $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic>
      $<$<CXX_COMPILER_ID:MSVC>:/W4>)

  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

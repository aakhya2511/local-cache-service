cmake_minimum_required(VERSION 3.20)
project(local_cache_service LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LCS_BUILD_TESTS "Build the test suite (requires GoogleTest)" ON)
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan are mutually exclusive; configure separate build directories.")
endif()

# The service uses epoll, eventfd, signalfd and AF_UNIX; there is no portable
# fallback and pretending otherwise would only hide breakage at runtime.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "local_cache_service targets Linux (epoll/eventfd/signalfd). "
    "Current system: ${CMAKE_SYSTEM_NAME}.")
endif()

add_library(lcs_flags INTERFACE)
target_compile_options(lcs_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2)
target_compile_features(lcs_flags INTERFACE cxx_std_20)

set(LCS_SANITIZER_FLAGS "")
if(ENABLE_ASAN)
  list(APPEND LCS_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(ENABLE_UBSAN)
  list(APPEND LCS_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined
       -fno-omit-frame-pointer)
endif()
if(ENABLE_TSAN)
  list(APPEND LCS_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()
if(LCS_SANITIZER_FLAGS)
  target_compile_options(lcs_flags INTERFACE ${LCS_SANITIZER_FLAGS} -g)
  target_link_options(lcs_flags INTERFACE ${LCS_SANITIZER_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Core library: cache, protocol, serialization, transport, worker pool.
# ---------------------------------------------------------------------------
add_library(cache_core STATIC
  src/lru_cache.cpp
  src/protocol.cpp
  src/serializer.cpp
  src/server.cpp
  src/connection.cpp
  src/thread_pool.cpp
  src/config.cpp
  src/logging.cpp
)
target_include_directories(cache_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cache_core PUBLIC lcs_flags Threads::Threads)

# ---------------------------------------------------------------------------
# Client library (usable independently of the server code).
# ---------------------------------------------------------------------------
add_library(cache_client_lib STATIC
  client/cache_client.cpp
)
target_include_directories(cache_client_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/client)
target_link_libraries(cache_client_lib PUBLIC cache_core)

add_executable(cache_server src/main.cpp)
target_link_libraries(cache_server PRIVATE cache_core)

add_executable(cache_client client/client_main.cpp)
target_link_libraries(cache_client PRIVATE cache_client_lib)

add_executable(cache_benchmark benchmark/benchmark.cpp benchmark/async_client.cpp)
target_include_directories(cache_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark)
target_link_libraries(cache_benchmark PRIVATE cache_client_lib)

add_executable(cache_stress benchmark/stress.cpp benchmark/async_client.cpp)
target_include_directories(cache_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark)
target_link_libraries(cache_stress PRIVATE cache_client_lib)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LCS_BUILD_TESTS)
  include(FetchContent)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(cache_tests
    tests/lru_cache_test.cpp
    tests/protocol_test.cpp
    tests/serialization_test.cpp
    tests/server_test.cpp
    tests/concurrency_test.cpp
    tests/memory_bound_test.cpp
  )
  target_link_libraries(cache_tests PRIVATE cache_client_lib GTest::gtest GTest::gtest_main)
  target_compile_definitions(cache_tests PRIVATE
    LCS_SERVER_BINARY="$<TARGET_FILE:cache_server>")
  add_dependencies(cache_tests cache_server)

  include(GoogleTest)
  # PRE_TEST avoids executing sanitizer-instrumented binaries during the build;
  # some container runtimes cannot reserve TSan's shadow address space while
  # CMake is linking/discovering tests.
  gtest_discover_tests(cache_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
endif()

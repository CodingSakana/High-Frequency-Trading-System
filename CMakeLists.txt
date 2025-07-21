cmake_minimum_required(VERSION 3.12)

project(LowLatencyApp CXX)

option(PERF_TEST "Enable performance tests" OFF)
if(PERF_TEST)
    add_compile_definitions(PERF)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_COMPILER g++)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -Wall -Wextra -Werror -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra -Werror -Wpedantic")

set(CMAKE_VERBOSE_MAKEFILE on)

add_subdirectory(common)
add_subdirectory(exchange)
add_subdirectory(trading)

set(LIBS "")

list(APPEND LIBS libexchange)
list(APPEND LIBS libtrading)
list(APPEND LIBS libcommon)
list(APPEND LIBS pthread)

add_executable(exchange_main exchange/exchange_main.cpp)
target_link_libraries(exchange_main PUBLIC ${LIBS})

add_executable(trading_main trading/trading_main.cpp)
target_link_libraries(trading_main PUBLIC ${LIBS})

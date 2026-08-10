cmake_minimum_required(VERSION 3.20)
project(sshos CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -g -O0)
  add_link_options(-fsanitize=address,undefined)
else()
  add_compile_options(-O2)
endif()

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_library(sshos_core STATIC ${CORE_SOURCES})
target_include_directories(sshos_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

add_executable(sshos src/main.cpp)
target_link_libraries(sshos PRIVATE sshos_core)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.cpp)
add_executable(sshos_tests tests/main.cpp ${TEST_SOURCES})
target_include_directories(sshos_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(sshos_tests PRIVATE sshos_core)

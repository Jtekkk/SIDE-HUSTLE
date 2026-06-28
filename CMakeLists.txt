cmake_minimum_required(VERSION 3.20)
project(side_hustle CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_executable(side-hustle ${SOURCES})
target_include_directories(side-hustle PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(side-hustle PRIVATE -Wall -Wextra -O2)

# core/ — the C/C++ side of RLtvOS that is built with CMake (tvOS toolchain)
# and linked into the Xcode app as static libraries: rlcore now (bridge +
# self-tests), FEXCore + the fake kernel next.
cmake_minimum_required(VERSION 3.22)
project(rlcore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RL_ENABLE_FEX "Build FEXCore and the fake kernel" OFF)

add_library(rlcore STATIC
  src/rlcore.cpp
)
target_include_directories(rlcore PUBLIC include)
target_compile_options(rlcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(rlcore PRIVATE RLCORE_VERSION="0.1")

if(RL_ENABLE_FEX)
  add_subdirectory(fex)
endif()

install(TARGETS rlcore ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

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
if(CMAKE_CROSSCOMPILING)
  set(_rl_tests_default OFF)
else()
  set(_rl_tests_default ON)
endif()
option(RL_BUILD_TESTS "Build host-side tests (Linux/macOS)" ${_rl_tests_default})

add_subdirectory(vfs)

add_library(rlcore STATIC
  src/rlcore.cpp
  src/rlcore_vfs.cpp
)
target_include_directories(rlcore PUBLIC include)
target_compile_options(rlcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(rlcore PRIVATE RLCORE_VERSION="0.1")
target_link_libraries(rlcore PUBLIC rlvfs)

if(RL_ENABLE_FEX)
  add_subdirectory(fex)
endif()

if(RL_BUILD_TESTS)
  add_executable(vfs_test tests/vfs_test.cpp)
  target_link_libraries(vfs_test PRIVATE rlvfs)
  enable_testing()
  add_test(NAME vfs_test COMMAND vfs_test ${CMAKE_SOURCE_DIR}/..)
endif()

install(TARGETS rlcore ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

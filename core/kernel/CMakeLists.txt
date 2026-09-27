# core/kernel — the fake Linux kernel (Phase C): the Linux x86-64 syscall ABI
# on XNU behind FEXCore's SyscallHandler. Two libraries:
#   rlkernel_base  ELF loader, address space, fd table, log. No FEX, no
#                  network: built and unit-tested on the Linux host too.
#   rlkernel       processes/threads, syscall table, C API (rlkernel.h).
#                  Needs FEXCore's headers and rlvfs's headers; built when this
#                  directory is included from core/fex (tvOS + x86-64 compile
#                  check); the app links it next to librlfex_all.a.
cmake_minimum_required(VERSION 3.22)

add_library(rlkernel_base STATIC
  src/log.cpp
  src/mm.cpp
  src/elf_loader.cpp
  src/fds.cpp
  src/pipe.cpp
  src/futex.cpp
  src/overlay.cpp
  src/poll.cpp
  src/socket.cpp
  src/locks.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(rlkernel_base PUBLIC Threads::Threads)
target_include_directories(rlkernel_base PUBLIC include src)
target_compile_features(rlkernel_base PUBLIC cxx_std_20)
target_compile_options(rlkernel_base PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)

if(TARGET FEXCore)
  add_library(rlkernel STATIC
    src/process.cpp
    src/signals.cpp
    src/syscalls.cpp
    src/syscalls_fs.cpp
    src/syscalls_io.cpp
    src/timers.cpp
    src/rlkernel.cpp
  )
  target_include_directories(rlkernel
    PUBLIC include
    PRIVATE src
            ${CMAKE_CURRENT_SOURCE_DIR}/../fex/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../fex/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../vfs/include
            ${FEX_ROOT}/Source)
  target_compile_options(rlkernel PRIVATE -Wall -Wno-unused-parameter -fno-exceptions -fno-strict-aliasing)
  target_compile_definitions(rlkernel PRIVATE ${DEFINES} "FEXCORE_PRESERVE_ALL_ATTR=" "FEXCORE_HAS_PRESERVE_ALL_ATTR=0")
  target_link_libraries(rlkernel PUBLIC rlkernel_base FEXCore FEXCore_Base)
endif()

if(RL_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(kernel_test tests/kernel_test.cpp)
  target_link_libraries(kernel_test PRIVATE rlkernel_base)
  add_test(NAME kernel_test COMMAND kernel_test)
endif()

# core/fex — builds the vendored FEXCore (third_party/FEX @ 59f85d6) as static
# libraries for tvOS, plus RLtvOS's Darwin platform layer (rlfex).
#
# This file replaces FEX's root CMakeLists.txt, which refuses anything but
# Linux/Windows. It reproduces the variables, defines and external targets
# FEXCore/ expects, then adds FEXCore/ itself. Host-side compile checks on
# x86-64 Linux use the same file (FEX's own "x86 host debug" configuration).
cmake_minimum_required(VERSION 3.22)
project(rlfex C CXX ASM)

get_filename_component(FEX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/FEX" ABSOLUTE)

include(CheckCXXSourceCompiles)
include(CheckCXXCompilerFlag)
include(CheckPIESupported)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

# ---- FEX options (subset) ---------------------------------------------------
option(ENABLE_ASSERTIONS "FEX debug assertions" ON)
set(ENABLE_FEX_ALLOCATOR TRUE)
set(ENABLE_JEMALLOC_GLIBC_ALLOC FALSE)
set(ENABLE_VIXL_DISASSEMBLER FALSE)
set(ENABLE_VIXL_SIMULATOR FALSE)
set(ENABLE_ZYDIS FALSE)
set(ENABLE_FEXCORE_PROFILER FALSE)
set(ENABLE_GLIBC_ALLOCATOR_HOOK_FAULT FALSE)
set(BUILD_STEAM_SUPPORT FALSE)
set(HAS_CLANG_PRESERVE_ALL FALSE)   # revisit as an optimisation once running

# ---- architecture ------------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" processor)
if(processor MATCHES "x86|amd64")
  set(ARCHITECTURE_x86_64 1)
  add_compile_definitions(ARCHITECTURE_x86_64=1)
  # -mavx: the interpreter fallbacks build __m256i register pairs on an x86 host.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcx16 -mavx")
  message(STATUS "rlfex: x86-64 host-debug build (compile check only)")
elseif(processor MATCHES "^aarch64|^arm64")
  set(ARCHITECTURE_arm64 1)
  add_compile_definitions(ARCHITECTURE_arm64=1)
else()
  message(FATAL_ERROR "rlfex: unsupported processor ${processor}")
endif()

set(FEX_TUNE_COMPILE_FLAGS)
if(ARCHITECTURE_arm64)
  # A15 (Apple TV 4K 3rd gen). armv8.5-a keeps FEX's assumptions (crc, lse, ...).
  check_cxx_compiler_flag("-mcpu=apple-a15" RL_HAS_APPLE_A15)
  if(RL_HAS_APPLE_A15 AND APPLE)
    list(APPEND FEX_TUNE_COMPILE_FLAGS "-mcpu=apple-a15")
  else()
    list(APPEND FEX_TUNE_COMPILE_FLAGS "-march=armv8-a+crc")
  endif()
endif()

# ---- global defines FEX's root normally adds ----------------------------------
if(ENABLE_ASSERTIONS)
  add_compile_definitions(ASSERTIONS_ENABLED=1)
endif()
add_compile_definitions(_LARGEFILE64_SOURCE FEX_DISABLE_TELEMETRY=1
                        GLOBAL_DATA_DIRECTORY="/rltvos/share/fex-emu/"
                        "FEX_PRESERVE_ALL_ATTR=" "FEX_HAS_PRESERVE_ALL_ATTR=0"
                        HAS_PROGRAM_INVOCATION_NAME=0)
add_compile_options(-Wno-trigraphs -Wall)
check_cxx_compiler_flag(-Wno-deprecated-enum-enum-conversion ENUM_ENUM_WARNING)
if(ENUM_ENUM_WARNING)
  add_compile_options(-Wno-deprecated-enum-enum-conversion)
endif()

# git_version.h.in inputs
set(GIT_DESCRIBE_STRING "FEX-59f85d6-rltvos")
set(GIT_HASH "59f85d6b7df4eb053d2cc1b9d33411b4f31bfbaa")
string(REGEX REPLACE "(..|.$)" "0x\\1, " GIT_HASH_ARRAY "${GIT_HASH}")

# FEX's LinkerGC adds GNU ld flags; Apple ld does not take them and we only
# build static archives here.
macro(LinkerGC target)
endmacro()

find_package(Python 3.9 REQUIRED COMPONENTS Interpreter)

# ---- externals ---------------------------------------------------------------
add_subdirectory(${FEX_ROOT}/External/unordered_dense ext/unordered_dense)
set(XXHASH_BUNDLED_MODE TRUE)
set(XXHASH_BUILD_XXHSUM FALSE)
add_subdirectory(${FEX_ROOT}/External/xxhash/cmake_unofficial ext/xxhash)
set(FMT_INSTALL OFF)
add_subdirectory(${FEX_ROOT}/External/fmt ext/fmt)
add_subdirectory(${FEX_ROOT}/External/SoftFloat-3e ext/softfloat)
add_subdirectory(${FEX_ROOT}/External/cephes ext/cephes)
add_subdirectory(${FEX_ROOT}/External/rpmalloc ext/rpmalloc)
add_subdirectory(${FEX_ROOT}/FEXHeaderUtils ext/fhu)
add_subdirectory(${FEX_ROOT}/CodeEmitter ext/codeemitter)

include_directories(${FEX_ROOT}/Source)

# ---- FEXCore -----------------------------------------------------------------
add_subdirectory(${FEX_ROOT}/FEXCore fexcore)
if(TARGET FEXCore_shared)
  set_target_properties(FEXCore_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# ---- rlfex: Darwin platform layer + bare-function runner -----------------------
# Built on Apple only; the x86-64 host-debug build stops at FEXCore itself.
if(APPLE)
  add_library(rlfex STATIC
    src/rlfex.cpp
    src/darwin_platform.cpp
    ${FEX_ROOT}/Source/Tools/CommonTools/DummyHandlers.cpp
  )
  target_include_directories(rlfex PUBLIC include PRIVATE src ${FEX_ROOT}/Source ${FEX_ROOT}/Source/Tools/CommonTools)
  target_compile_options(rlfex PRIVATE -Wall -Wno-unused-parameter -fno-exceptions -fno-strict-aliasing)
  target_compile_definitions(rlfex PRIVATE ${DEFINES} "FEXCORE_PRESERVE_ALL_ATTR=" "FEXCORE_HAS_PRESERVE_ALL_ATTR=0"
                                          RLFEX_FEX_VERSION="${GIT_DESCRIBE_STRING}")
  target_link_libraries(rlfex PUBLIC FEXCore FEXCore_Base JemallocLibs rpmalloc fmt::fmt xxHash::xxhash
                                     cephes_128bit softfloat_3e unordered_dense::unordered_dense FEXHeaderUtils CodeEmitter)

  # The list of archives the app has to link (written for the CI packaging step).
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/rlfex-archives.txt CONTENT
    "$<TARGET_FILE:rlfex>\n$<TARGET_FILE:FEXCore>\n$<TARGET_FILE:FEXCore_Base>\n$<TARGET_FILE:JemallocLibs>\n$<TARGET_FILE:rpmalloc>\n$<TARGET_FILE:fmt>\n$<TARGET_FILE:xxhash>\n$<TARGET_FILE:cephes_128bit>\n$<TARGET_FILE:softfloat_3e>\n")
endif()

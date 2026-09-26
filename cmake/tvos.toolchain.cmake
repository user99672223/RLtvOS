# tvos.toolchain.cmake — cross-compile C/C++ static libraries for tvOS (arm64)
# with the Xcode toolchain on the macOS runner. Used for FEXCore + the fake
# kernel; the resulting .a files are linked into the Xcode app.
#
#   cmake -S core -B build/core -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/tvos.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
set(CMAKE_SYSTEM_NAME tvOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_SYSROOT appletvos CACHE STRING "" FORCE)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "" FORCE)
endif()

execute_process(COMMAND xcrun --sdk appletvos --show-sdk-path
                OUTPUT_VARIABLE _tvos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND xcrun --sdk appletvos --find clang
                OUTPUT_VARIABLE _tvos_cc OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND xcrun --sdk appletvos --find clang++
                OUTPUT_VARIABLE _tvos_cxx OUTPUT_STRIP_TRAILING_WHITESPACE)
set(CMAKE_C_COMPILER "${_tvos_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_tvos_cxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER "${_tvos_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_SYSROOT "${_tvos_sdk}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${_tvos_sdk}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Only static libraries are built for the app; bitcode is dead.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO)
set(CMAKE_MACOSX_BUNDLE OFF)

# Common flags: the app is arm64 (not arm64e), 16 KB pages, no exceptions
# needed by FEXCore but keep defaults unless a project overrides them.
set(_tvos_common "-target arm64-apple-tvos${CMAKE_OSX_DEPLOYMENT_TARGET} -isysroot ${_tvos_sdk} -fvisibility=hidden")
set(CMAKE_C_FLAGS_INIT "${_tvos_common}")
set(CMAKE_CXX_FLAGS_INIT "${_tvos_common}")
set(CMAKE_ASM_FLAGS_INIT "${_tvos_common}")

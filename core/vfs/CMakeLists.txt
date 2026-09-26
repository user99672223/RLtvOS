# rlvfs — HTTP range client, manifest, block cache, guest namespace.
add_library(rlvfs STATIC
  src/http.cpp
  src/manifest.cpp
  src/blockcache.cpp
  src/vfs.cpp
)
target_include_directories(rlvfs PUBLIC include)
target_compile_options(rlvfs PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(rlvfs PUBLIC z)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(rlvfs PUBLIC pthread)
endif()
install(TARGETS rlvfs ARCHIVE DESTINATION lib)
